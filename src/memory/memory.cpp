/*
 * Copyright (c) 2018, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @brief Device Memory Manager implementation. 
 *
 * Efficient allocation, deallocation and tracking of GPU memory.
 *
 */

#include "rmm.h"
#include "memory_manager.h"
#include <fstream>
#include <sstream>

/** ---------------------------------------------------------------------------*
 * @brief Macro wrapper for RMM API calls to return appropriate RMM errors.
 * ---------------------------------------------------------------------------**/
#define RMM_CHECK_CUDA(call) do { \
    cudaError_t cudaError = (call); \
    if( cudaError == cudaErrorMemoryAllocation ) { \
        return RMM_ERROR_OUT_OF_MEMORY; \
    } \
    else if( cudaError != cudaSuccess ) { \
        return RMM_ERROR_CUDA_ERROR; \
    } \
} while(0)

// Global instance of the log
rmm::Logger theLog;

// RAII logger class
class LogIt
{
public:
    LogIt(rmm::Logger::MemEvent_t event, size_t size, cudaStream_t stream) 
    : event(event), device(0), ptr(0), size(size), stream(stream)
    {
        cudaGetDevice(&device);
        start = std::chrono::system_clock::now();
    }

    LogIt(rmm::Logger::MemEvent_t event, void* ptr, size_t size, cudaStream_t stream) 
    : event(event), device(0), ptr(ptr), size(size), stream(stream)
    {
        cudaGetDevice(&device);
        start = std::chrono::system_clock::now();
    }

    /// Sometimes you need to start logging before the pointer address is known
    void setPointer(void* p) { ptr = p; }

    ~LogIt() 
    {
        auto end = std::chrono::system_clock::now();
        theLog.record(event, device, ptr, start, end, size, stream); 
    }

private:
    rmm::Logger::MemEvent_t event;
    int device;
    void* ptr;
    size_t size;
    cudaStream_t stream;
    rmm::Logger::TimePt start;
};


// Initialize memory manager state and storage.
rmmError_t rmmInitialize()
{
    RMM_CHECK_CUDA(cudaFree(0));
    return RMM_SUCCESS;
}

// Shutdown memory manager.
rmmError_t rmmFinalize()
{
    return RMM_SUCCESS;
}
 
// Allocate memory and return a pointer to device memory. 
rmmError_t rmmAlloc(void **ptr, size_t size, cudaStream_t stream)
{
    LogIt log(rmm::Logger::Alloc, size, stream);
	if (!ptr && !size) {
        return RMM_SUCCESS;
    }
    else if (!size) {
        ptr[0] = NULL;
        return RMM_SUCCESS;
    }

    if (!ptr) 
    	return RMM_ERROR_INVALID_ARGUMENT;

    RMM_CHECK_CUDA(cudaMalloc(ptr, size));
    log.setPointer(*ptr);

    return RMM_SUCCESS;
}

// Reallocate device memory block to new size and recycle any remaining memory.
rmmError_t rmmRealloc(void **ptr, size_t new_size, cudaStream_t stream)
{
    LogIt log(rmm::Logger::Realloc, new_size, stream);
	if (!ptr && !new_size) {
        return RMM_SUCCESS;
    }

    if (!ptr) 
    	return RMM_ERROR_INVALID_ARGUMENT;

	RMM_CHECK_CUDA(cudaFree(*ptr));
	RMM_CHECK_CUDA(cudaMalloc(ptr, new_size));
    log.setPointer(ptr);

    return RMM_SUCCESS;
}

// Release device memory and recycle the associated memory.
rmmError_t rmmFree(void *ptr, cudaStream_t stream)
{
    LogIt log(rmm::Logger::Free, ptr, 0, stream);
	RMM_CHECK_CUDA(cudaFree(ptr));
	return RMM_SUCCESS;
}

// Get amounts of free and total memory managed by a manager associated with the stream.
rmmError_t rmmGetInfo(size_t *freeSize, size_t *totalSize, cudaStream_t stream)
{
	RMM_CHECK_CUDA(cudaMemGetInfo(freeSize, totalSize));
	return RMM_SUCCESS;
}

// Write the memory event stats log to specified path/filename
rmmError_t rmmWriteLog(const char* filename)
{
    try 
    {
        std::ofstream csv;
        csv.open(filename);
        theLog.to_csv(csv);
    }
    catch (const std::ofstream::failure& e) {
        return RMM_ERROR_IO;
    }
    return RMM_SUCCESS;
}

size_t rmmLogSize()
{
    std::ostringstream csv; 
    theLog.to_csv(csv);
    return csv.str().size();
}

rmmError_t rmmGetLog(char *buffer, size_t buffer_size)
{
    try 
    {
        std::ostringstream csv; 
        theLog.to_csv(csv);
        csv.str().copy(buffer, std::min(buffer_size, csv.str().size()));
    }
    catch (const std::ofstream::failure& e) {
        return RMM_ERROR_IO;
    }
    return RMM_SUCCESS;
}

