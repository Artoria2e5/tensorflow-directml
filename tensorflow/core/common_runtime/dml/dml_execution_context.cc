/* Copyright (c) Microsoft Corporation.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "dml_execution_context.h"

#include "dml_bfc_allocator.h"
#include "dml_buffer.h"
#include "dml_tracing.h"
#include "dml_util.h"
#include "tensorflow/core/util/env_var.h"

namespace tensorflow {

DmlExecutionContext::DmlExecutionContext(ID3D12Device* d3d_device,
                                         IDMLDevice* dml_device,
                                         ID3D12CommandQueue* queue,
                                         DmlAllocator* allocator) {
  impl_ = absl::make_unique<DmlExecutionContextImpl>(d3d_device, dml_device,
                                                     queue, allocator);

  shared_state_ = std::make_shared<SharedState>();
  shared_state_->next_flush_event = impl_->GetCurrentCompletionEvent();
  ++shared_state_->next_flush_event.fence_value;

  uint32_t batch_flush_size = default_batch_flush_size;
  {
    int64 batch_flush_size_int64 = 0;
    Status s = ReadInt64FromEnvVar("TF_DIRECTML_BATCH_FLUSH_SIZE", 0,
                                   &batch_flush_size_int64);
    if (s.ok() && batch_flush_size_int64 != 0) {
      batch_flush_size = static_cast<uint32_t>(batch_flush_size_int64);
    }
  }

  uint32_t batch_flush_time_us = default_batch_flush_time_us;
  {
    int64 batch_flush_time_us_int64 = 0;
    Status s = ReadInt64FromEnvVar("TF_DIRECTML_BATCH_FLUSH_TIME", 0,
                                   &batch_flush_time_us_int64);
    if (s.ok() && batch_flush_time_us_int64 != 0) {
      batch_flush_time_us = static_cast<uint32_t>(batch_flush_time_us_int64);
    }
  }

  // Launch the thread, supplying it with a pointer to the shared state
  thread_ = std::thread(ThreadProc, shared_state_, impl_.get(),
                        batch_flush_size, batch_flush_time_us);
}

DmlExecutionContext::~DmlExecutionContext() {
  // Request exit of the background thread
  std::unique_lock<std::mutex> lock(shared_state_->mutex);
  shared_state_->exit_requested = true;
  shared_state_->new_function_enqueued.notify_all();  // wake the thread
  lock.unlock();

  // detach() rather than join(), because we don't want (or need) to wait for
  // it to complete. This prevents blocking in a destructor, which would be
  // bad.
  thread_.detach();
}

DmlExecutionContextImpl::DmlExecutionContextImpl(ID3D12Device* d3d_device,
                                                 IDMLDevice* dml_device,
                                                 ID3D12CommandQueue* queue,
                                                 DmlAllocator* allocator)
    : queue_(std::make_shared<DmlCommandQueue>(queue)),
      d3d_device_(d3d_device),
      dml_device_(dml_device),
      descriptor_pool_(d3d_device, 2048),
      allocator_(allocator),
      command_allocator_ring_(d3d_device, queue_->GetType(),
                              queue_->GetCurrentCompletionEvent()) {
  DML_CHECK_SUCCEEDED(
      dml_device->CreateCommandRecorder(IID_PPV_ARGS(&recorder_)));
  OpenCommandList();
}

DmlGpuEvent DmlExecutionContextImpl::CopyBufferRegion(
    ID3D12Resource* dst_buffer, uint64_t dst_offset,
    D3D12_RESOURCE_STATES dst_state, ID3D12Resource* src_buffer,
    uint64_t src_offset, D3D12_RESOURCE_STATES src_state, uint64_t byte_count) {
  if (!status_.ok()) {
    GetCurrentCompletionEvent();
  }

  DmlTracing::Instance().LogExecutionContextCopyBufferRegion();

  absl::InlinedVector<D3D12_RESOURCE_BARRIER, 3> barriers;

  if (!(dst_state & D3D12_RESOURCE_STATE_COPY_DEST)) {
    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
        dst_buffer, dst_state, D3D12_RESOURCE_STATE_COPY_DEST));
  }
  if (!(src_state & D3D12_RESOURCE_STATE_COPY_SOURCE)) {
    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
        src_buffer, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE));
  }

  if (!barriers.empty()) {
    current_command_list_->ResourceBarrier(barriers.size(), barriers.data());
  }

  current_command_list_->CopyBufferRegion(dst_buffer, dst_offset, src_buffer,
                                          src_offset, byte_count);

  // Reset barrier state
  for (auto& barrier : barriers) {
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  }

  // Since this copy may write to GPU memory, we also need to perform an
  // aliasing barrier
  barriers.push_back(CD3DX12_RESOURCE_BARRIER::Aliasing(nullptr, nullptr));

  current_command_list_->ResourceBarrier(barriers.size(), barriers.data());

  OnCommandRecorded();

  return GetCurrentCompletionEvent();
}

DmlGpuEvent DmlExecutionContextImpl::FillBufferWithPattern(
    ID3D12Resource* dst, uint64_t dst_offset, uint64_t dst_size_in_bytes,
    absl::Span<const uint8_t>
        value /* Data type agnostic value, treated as raw bits */) {
  if (!status_.ok()) {
    GetCurrentCompletionEvent();
  }

  DmlTracing::Instance().LogExecutionContextFillBufferWithPattern();

  // The fill pattern for ClearUnorderedAccessViewUint is 16 bytes.
  union {
    uint32_t integers[4];
    uint8_t bytes[16];
  } fillPattern = {};

  assert(ARRAYSIZE(fillPattern.bytes) == 16);
  assert(value.size() <=
         ARRAYSIZE(fillPattern.bytes));  // No element is expected larger than
                                         // 128 bits (e.g. complex128).

  if (!value.empty()) {
    assert(ARRAYSIZE(fillPattern.bytes) % value.size() ==
           0);  // Should fit evenly into 16 bytes (e.g. uint8, float16, uint32,
                // float64...).

    // Repeat the value multiple times into the pattern buffer.
    size_t valueIndex = 0;
    for (uint8_t& p : fillPattern.bytes) {
      p = value[valueIndex++];
      valueIndex = (valueIndex == value.size()) ? 0 : valueIndex;
    }
  }
  // Else just leave fill pattern as zeroes.

  // The destination must be appropriately aligned and padded
  assert(dst_offset % sizeof(uint32_t) == 0);
  assert(dst_size_in_bytes % sizeof(uint32_t) == 0);

  // Create a RAW buffer UAV over the resource.
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.Buffer.FirstElement =
      static_cast<uint32_t>(dst_offset / sizeof(uint32_t));
  uav_desc.Buffer.NumElements =
      static_cast<uint32_t>(dst_size_in_bytes / sizeof(uint32_t));
  uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

  const uint32_t needed_descriptor_count = 1;
  DmlDescriptorRange descriptor_range_cpu = descriptor_pool_.AllocDescriptors(
      needed_descriptor_count, queue_->GetNextCompletionEvent(),
      D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
  DmlDescriptorRange descriptor_range_gpu = descriptor_pool_.AllocDescriptors(
      needed_descriptor_count, queue_->GetNextCompletionEvent(),
      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
  d3d_device_->CreateUnorderedAccessView(dst, nullptr, &uav_desc,
                                         descriptor_range_cpu.cpu_handle);
  d3d_device_->CreateUnorderedAccessView(dst, nullptr, &uav_desc,
                                         descriptor_range_gpu.cpu_handle);

  SetDescriptorHeap(descriptor_range_gpu.heap);

  // Record a ClearUAV onto the command list.
  current_command_list_->ClearUnorderedAccessViewUint(
      descriptor_range_gpu.gpu_handle, descriptor_range_cpu.cpu_handle, dst,
      fillPattern.integers, 0, nullptr);

  // Barrier all outputs.
  D3D12_RESOURCE_BARRIER barriers[] = {
      CD3DX12_RESOURCE_BARRIER::UAV(nullptr),
      CD3DX12_RESOURCE_BARRIER::Aliasing(nullptr, nullptr)};
  current_command_list_->ResourceBarrier(ABSL_ARRAYSIZE(barriers), barriers);

  OnCommandRecorded();

  return GetCurrentCompletionEvent();
}

DmlGpuEvent DmlExecutionContextImpl::InitializeOperator(
    IDMLOperatorInitializer* initializer, IDMLBindingTable* binding_table,
    ID3D12DescriptorHeap* descriptor_heap) {
  if (!status_.ok()) {
    GetCurrentCompletionEvent();
  }

  // Record the initialization work.
  SetDescriptorHeap(descriptor_heap);
  recorder_->RecordDispatch(current_command_list_.Get(), initializer,
                            binding_table);

  // Barrier if there's an output (i.e. persistent resource), or if any temps
  // are used.
  DML_BINDING_PROPERTIES binding_props = initializer->GetBindingProperties();
  if ((binding_props.PersistentResourceSize > 0) ||
      (binding_props.TemporaryResourceSize > 0)) {
    D3D12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::UAV(nullptr),
        CD3DX12_RESOURCE_BARRIER::Aliasing(nullptr, nullptr)};
    current_command_list_->ResourceBarrier(ABSL_ARRAYSIZE(barriers), barriers);
  }

  OnCommandRecorded();

  return GetCurrentCompletionEvent();
}

DmlGpuEvent DmlExecutionContextImpl::ExecuteOperator(
    IDMLCompiledOperator* op, IDMLBindingTable* binding_table,
    ID3D12DescriptorHeap* descriptor_heap) {
  if (!status_.ok()) {
    GetCurrentCompletionEvent();
  }

  // Record the execution work.
  SetDescriptorHeap(descriptor_heap);
  recorder_->RecordDispatch(current_command_list_.Get(), op, binding_table);

  // Barrier all outputs.
  D3D12_RESOURCE_BARRIER barriers[] = {
      CD3DX12_RESOURCE_BARRIER::UAV(nullptr),
      CD3DX12_RESOURCE_BARRIER::Aliasing(nullptr, nullptr)};
  current_command_list_->ResourceBarrier(ABSL_ARRAYSIZE(barriers), barriers);

  OnCommandRecorded();

  return GetCurrentCompletionEvent();
}

DmlGpuEvent DmlExecutionContextImpl::ResourceBarrier(
    absl::Span<const D3D12_RESOURCE_BARRIER> barriers) {
  if (!status_.ok()) {
    GetCurrentCompletionEvent();
  }

  current_command_list_->ResourceBarrier(static_cast<uint32_t>(barriers.size()),
                                         barriers.data());
  OnCommandRecorded();

  return GetCurrentCompletionEvent();
}

DmlGpuEvent DmlExecutionContextImpl::UavBarrier() {
  if (!status_.ok()) {
    GetCurrentCompletionEvent();
  }

  D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
  current_command_list_->ResourceBarrier(1, &barrier);
  OnCommandRecorded();

  return GetCurrentCompletionEvent();
}

StatusOr<DmlGpuEvent> DmlExecutionContextImpl::Flush() {
  DmlTracing::Instance().LogExecutionContextFlush();

  if (operations_recorded_in_current_command_list_ == 0) {
    // Nothing to flush
    return GetCurrentCompletionEvent();
  }

  CloseCommandListAndExecute();

  if (!status_.ok()) {
    // "Unknown" represents device removals, which are uncoverable failures
    if (!errors::IsUnknown(status_)) {
      status_ = Status::OK();
    }
    return status_;
  }

  return GetCurrentCompletionEvent();
}

DmlGpuEvent DmlExecutionContextImpl::GetCurrentCompletionEvent() {

  DmlGpuEvent event = queue_->GetCurrentCompletionEvent();

  // If something has been recorded into a command list but not submitted yet,
  // it means that the *next* fence value is the one to signal completion.
  if (operations_recorded_in_current_command_list_ != 0) {
    ++event.fence_value;
  }

  return event;
}

D3D12_COMMAND_LIST_TYPE DmlExecutionContextImpl::GetCommandListTypeForQueue()
    const {
  return queue_->GetType();
}

void DmlExecutionContextImpl::SetDescriptorHeap(
    ID3D12DescriptorHeap* descriptor_heap) {
  // This should have been checked in one of the public functions before calling
  // SetDescriptorHeap()
  DCHECK(status_.ok());

  if (descriptor_heap != nullptr &&
      descriptor_heap != current_descriptor_heap_) {
    current_descriptor_heap_ = descriptor_heap;

    ID3D12DescriptorHeap* descriptor_heaps[] = {descriptor_heap};
    current_command_list_->SetDescriptorHeaps(ABSL_ARRAYSIZE(descriptor_heaps),
                                              descriptor_heaps);
  }
}

void DmlExecutionContextImpl::OnCommandRecorded() {
  // This should have been checked in one of the public functions before calling
  // OnCommandRecorded()
  DCHECK(status_.ok());

  ++operations_recorded_in_current_command_list_;
}

void DmlExecutionContextImpl::OpenCommandList() {
  // This should have been checked in one of the public functions before calling
  // OpenCommandList()
  DCHECK(status_.ok());

  assert(current_descriptor_heap_ == nullptr);

  ID3D12CommandAllocator* allocator =
      command_allocator_ring_.GetCurrentAllocator();

  if (cached_command_lists_.empty()) {
    DML_CHECK_SUCCEEDED(d3d_device_->CreateCommandList(
        0, queue_->GetType(), command_allocator_ring_.GetCurrentAllocator(),
        nullptr, IID_PPV_ARGS(&current_command_list_)));
  } else {
    current_command_list_ = cached_command_lists_.front();
    cached_command_lists_.pop_front();
    DML_CHECK_SUCCEEDED(current_command_list_->Reset(allocator, nullptr));
  }

  // The current command allocator will become eligible for reset once this
  // command list completes execution
  command_allocator_ring_.AdvanceAllocator(queue_->GetNextCompletionEvent());
}

void DmlExecutionContextImpl::CloseCommandListAndExecute() {
  if (!status_.ok()) return;

  HRESULT hr = current_command_list_->Close();

  if (dml_util::HrIsOutOfMemory(hr)) {
    status_ = errors::ResourceExhausted("OOM when closing the command list");
  } else {
    DML_CHECK_SUCCEEDED(hr);

    if (operations_recorded_in_current_command_list_ != 0) {
      // Close and execute the command list
      ID3D12CommandList* commandLists[] = {current_command_list_.Get()};
      queue_->ExecuteCommandLists(commandLists);
    }

    cached_command_lists_.push_back(current_command_list_.Get());
  }

  current_command_list_ = nullptr;
  operations_recorded_in_current_command_list_ = 0;

  // The descriptor heap must be set on the command list the next time it's
  // opened.
  current_descriptor_heap_ = nullptr;

  // Fail early if something horrifying happens
  DML_CHECK_SUCCEEDED(dml_device_->GetDeviceRemovedReason());
  DML_CHECK_SUCCEEDED(d3d_device_->GetDeviceRemovedReason());

  // Always keep the command list in an opened state
  OpenCommandList();
}

DmlGpuEvent DmlExecutionContext::CopyBufferRegion(
    ID3D12Resource* dst_buffer, uint64_t dst_offset,
    D3D12_RESOURCE_STATES dst_state, ID3D12Resource* src_buffer,
    uint64_t src_offset, D3D12_RESOURCE_STATES src_state, uint64_t byte_count) {
  std::unique_lock<std::mutex> lock(shared_state_->mutex);

  shared_state_->WriteBatch().emplace_back([=]() {
    impl_->CopyBufferRegion(dst_buffer, dst_offset, dst_state, src_buffer,
                            src_offset, src_state, byte_count);
  });

  shared_state_->new_function_enqueued.notify_all();

  return shared_state_->next_flush_event;
}

DmlGpuEvent DmlExecutionContext::FillBufferWithPattern(
    ID3D12Resource* dst, uint64_t dst_offset, uint64_t dst_size_in_bytes,
    absl::Span<const uint8_t> value) {
  std::unique_lock<std::mutex> lock(shared_state_->mutex);

  absl::InlinedVector<uint8_t, 16> value_copy(value.size());
  std::copy(value.begin(), value.end(), value_copy.begin());

  shared_state_->WriteBatch().emplace_back(
      [=, value_copy = std::move(value_copy)]() {
        impl_->FillBufferWithPattern(dst, dst_offset, dst_size_in_bytes,
                                     value_copy);
      });

  return shared_state_->next_flush_event;
}

DmlGpuEvent DmlExecutionContext::InitializeOperator(
    IDMLOperatorInitializer* initializer,
    Microsoft::WRL::ComPtr<IDMLBindingTable>&& binding_table,
    ID3D12DescriptorHeap* descriptor_heap) {
  std::unique_lock<std::mutex> lock(shared_state_->mutex);

  shared_state_->WriteBatch().emplace_back(
      [=, binding_table = std::move(binding_table)]() {
        impl_->InitializeOperator(initializer, binding_table.Get(),
                                  descriptor_heap);
      });

  shared_state_->new_function_enqueued.notify_all();

  return shared_state_->next_flush_event;
}

DmlGpuEvent DmlExecutionContext::ExecuteOperator(
    IDMLCompiledOperator* op,
    Microsoft::WRL::ComPtr<IDMLBindingTable>&& binding_table,
    ID3D12DescriptorHeap* descriptor_heap) {
  std::unique_lock<std::mutex> lock(shared_state_->mutex);

  shared_state_->WriteBatch().emplace_back(
      [=, binding_table = std::move(binding_table)]() {
        impl_->ExecuteOperator(op, binding_table.Get(), descriptor_heap);
      });

  shared_state_->new_function_enqueued.notify_all();

  return shared_state_->next_flush_event;
}

DmlGpuEvent DmlExecutionContext::ResourceBarrier(
    absl::Span<const D3D12_RESOURCE_BARRIER> barriers) {
  std::unique_lock<std::mutex> lock(shared_state_->mutex);

  // The caller may not keep the barriers referenced by the span alive for
  // longer than this function call, so make a copy and transfer ownership to
  // the lambda.
  absl::InlinedVector<D3D12_RESOURCE_BARRIER, 4> barriers_copy;
  shared_state_->WriteBatch().emplace_back(
      [=, barriers = std::move(barriers_copy)]() {
        impl_->ResourceBarrier(barriers);
      });

  shared_state_->new_function_enqueued.notify_all();

  return shared_state_->next_flush_event;
}

DmlGpuEvent DmlExecutionContext::UavBarrier() {
  std::unique_lock<std::mutex> lock(shared_state_->mutex);

  shared_state_->WriteBatch().emplace_back([=]() { impl_->UavBarrier(); });

  shared_state_->new_function_enqueued.notify_all();

  return shared_state_->next_flush_event;
}

StatusOr<DmlGpuEvent> DmlExecutionContext::Flush() {
  std::unique_lock<std::mutex> lock(shared_state_->mutex);

  auto event = shared_state_->next_flush_event;
  if (shared_state_->WriteBatch().empty()) {
    --event.fence_value;
  }

  shared_state_->flush_requested = true;
  shared_state_->new_function_enqueued.notify_all();
  return event;
}

DmlGpuEvent DmlExecutionContext::GetCurrentCompletionEvent() {
  std::unique_lock<std::mutex> lock(shared_state_->mutex);
  auto event = shared_state_->next_flush_event;

  if (shared_state_->WriteBatch().empty()) {
    --event.fence_value;
  }

  return event;
}

/*static*/ void DmlExecutionContext::ThreadProc(
    std::shared_ptr<SharedState> state, DmlExecutionContextImpl* impl,
    uint32_t batch_flush_size, uint32_t batch_flush_time_us) {
  auto last_flush_time = std::chrono::high_resolution_clock::now();

  while (true) {
    std::chrono::duration<double> elapsed =
        std::chrono::high_resolution_clock::now() - last_flush_time;
    auto elapsed_us = elapsed.count() * 1e6;

    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->exit_requested) {
      break;
    }

    auto& batch = state->WriteBatch();

    if (batch.empty()) {
      // Wait for new work to be batched.
      state->new_function_enqueued.wait(lock);
      continue;
    }

    // Check if it's time to swap the write/execute batches and flush work to
    // the GPU: this occurs if a flush is explicitly requested, the batch has
    // reached a certain size, or enough time has elapsed since the last flush.
    // The goal here is to balance feeding the GPU work while the CPU is
    // processing more commands and avoiding many small packets.
    bool flush = false;
    if (state->flush_requested || batch.size() >= batch_flush_size ||
        elapsed_us >= batch_flush_time_us) {
      state->write_batch_index = (state->write_batch_index + 1) % 2;
      flush = true;
      ++state->next_flush_event.fence_value;
    }
    state->flush_requested = false;

    // Unlock to allow kernels to resume writing to the new write batch.
    lock.unlock();

    // Invoke the batched functions and submit the work to the GPU.
    if (flush) {
      for (auto& f : batch) {
        f();
      }
      batch.clear();
      impl->Flush();
      last_flush_time = std::chrono::high_resolution_clock::now();
    }
  }
}

}  // namespace tensorflow
