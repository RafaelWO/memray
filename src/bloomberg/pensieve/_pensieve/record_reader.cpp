#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

#include "Python.h"

#include "exceptions.h"
#include "hooks.h"
#include "interval_tree.h"
#include "logging.h"
#include "record_reader.h"
#include "records.h"
#include "source.h"

namespace pensieve::api {

using namespace tracking_api;
using namespace io;
using namespace exception;

namespace {  // unnamed

const char*
allocatorName(hooks::Allocator allocator)
{
    switch (allocator) {
        case hooks::Allocator::MALLOC:
            return "malloc";
        case hooks::Allocator::FREE:
            return "free";
        case hooks::Allocator::CALLOC:
            return "calloc";
        case hooks::Allocator::REALLOC:
            return "realloc";
        case hooks::Allocator::POSIX_MEMALIGN:
            return "posix_memalign";
        case hooks::Allocator::MEMALIGN:
            return "memalign";
        case hooks::Allocator::VALLOC:
            return "valloc";
        case hooks::Allocator::PVALLOC:
            return "pvalloc";
        case hooks::Allocator::MMAP:
            return "mmap";
        case hooks::Allocator::MUNMAP:
            return "munmap";
    }

    return nullptr;
}

}  // unnamed namespace

void
RecordReader::readHeader(HeaderRecord& header)
{
    if (!d_input->read(header.magic, sizeof(MAGIC)) || (memcmp(header.magic, MAGIC, sizeof(MAGIC)) != 0))
    {
        throw std::ios_base::failure(
                "The provided input file does not look like a binary generated by pensieve.");
    }
    d_input->read(reinterpret_cast<char*>(&header.version), sizeof(header.version));
    if (header.version != CURRENT_HEADER_VERSION) {
        throw std::ios_base::failure(
                "The provided input file is incompatible with this version of pensieve.");
    }
    header.command_line.reserve(4096);
    if (!d_input->read(reinterpret_cast<char*>(&header.native_traces), sizeof(header.native_traces))
        || !d_input->read(reinterpret_cast<char*>(&header.stats), sizeof(header.stats))
        || !d_input->getline(header.command_line, '\0'))
    {
        throw std::ios_base::failure("Failed to read input file.");
    }

    if (!d_input->read(reinterpret_cast<char*>(&header.pid), sizeof(header.pid))) {
        throw std::ios_base::failure("Failed to read tPID from input file.");
    }
}

RecordReader::RecordReader(std::unique_ptr<Source> source)
: d_input(std::move(source))
{
    readHeader(d_header);
}

void
RecordReader::close() noexcept
{
    d_input->close();
}

bool
RecordReader::isOpen() const noexcept
{
    return d_input->is_open();
}

bool
RecordReader::parseFramePush()
{
    FramePush record{};
    if (!d_input->read(reinterpret_cast<char*>(&record), sizeof(record))) {
        return false;
    }
    thread_id_t tid = record.tid;

    d_stack_traces[tid].push_back(record.frame_id);
    return true;
}

bool
RecordReader::parseFramePop()
{
    FramePop record{};
    if (!d_input->read(reinterpret_cast<char*>(&record), sizeof(record))) {
        return false;
    }
    thread_id_t tid = record.tid;

    assert(!d_stack_traces[tid].empty());
    while (record.count) {
        --record.count;
        d_stack_traces[tid].pop_back();
    }
    return true;
}

bool
RecordReader::parseFrameIndex()
{
    tracking_api::pyframe_map_val_t pyframe_val;
    if (!d_input->read(reinterpret_cast<char*>(&pyframe_val.first), sizeof(pyframe_val.first))
        || !d_input->getline(pyframe_val.second.function_name, '\0')
        || !d_input->getline(pyframe_val.second.filename, '\0')
        || !d_input->read(
                reinterpret_cast<char*>(&pyframe_val.second.parent_lineno),
                sizeof(pyframe_val.second.parent_lineno)))
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(d_mutex);
    auto iterator = d_frame_map.insert(pyframe_val);
    if (!iterator.second) {
        throw std::runtime_error("Two entries with the same ID found!");
    }
    return true;
}

bool
RecordReader::parseNativeFrameIndex()
{
    UnresolvedNativeFrame frame{};
    if (!d_input->read(reinterpret_cast<char*>(&frame), sizeof(UnresolvedNativeFrame))) {
        return false;
    }
    d_native_frames.emplace_back(frame);
    return true;
}

bool
RecordReader::parseAllocationRecord(AllocationRecord& record)
{
    return d_input->read(reinterpret_cast<char*>(&record), sizeof(AllocationRecord));
}

bool
RecordReader::parseSegmentHeader()
{
    std::string filename;
    uintptr_t addr;
    size_t num_segments;
    if (!d_input->getline(filename, '\0')
        || !d_input->read(reinterpret_cast<char*>(&num_segments), sizeof(num_segments))
        || !d_input->read(reinterpret_cast<char*>(&addr), sizeof(addr)))
    {
        return false;
    }

    std::vector<Segment> segments(num_segments);
    for (size_t i = 0; i < num_segments; i++) {
        Segment segment{};
        if (!parseSegment(segment)) {
            return false;
        }
        segments.emplace_back(segment);
    }
    std::lock_guard<std::mutex> lock(d_mutex);
    d_symbol_resolver.addSegments(filename, addr, segments);
    return true;
}

bool
RecordReader::parseSegment(Segment& segment)
{
    RecordType record_type;
    if (!d_input->read(reinterpret_cast<char*>(&record_type), sizeof(record_type))) {
        return false;
    }
    assert(record_type == RecordType::SEGMENT);
    if (!d_input->read(reinterpret_cast<char*>(&segment), sizeof(Segment))) {
        return false;
    }
    return true;
}

bool
RecordReader::parseThreadRecord()
{
    thread_id_t tid;
    std::string name;
    if (!d_input->read(reinterpret_cast<char*>(&tid), sizeof(thread_id_t))
        || !d_input->getline(name, '\0')) {
        return false;
    }
    d_thread_names[tid] = name;
    return true;
}

size_t
RecordReader::getAllocationFrameIndex(const AllocationRecord& record)
{
    auto stack = d_stack_traces.find(record.tid);
    if (stack == d_stack_traces.end()) {
        return 0;
    }
    correctAllocationFrame(stack->second, record.py_lineno);
    return d_tree.getTraceIndex(stack->second);
}

void
RecordReader::correctAllocationFrame(stack_t& stack, int lineno)
{
    if (stack.empty()) {
        return;
    }
    const Frame& partial_frame = d_frame_map.at(stack.back());
    Frame allocation_frame{
            partial_frame.function_name,
            partial_frame.filename,
            partial_frame.parent_lineno,
            lineno};
    auto [allocation_index, is_new_frame] = d_allocation_frames.getIndex(allocation_frame);
    if (is_new_frame) {
        std::lock_guard<std::mutex> lock(d_mutex);
        d_frame_map.emplace(allocation_index, allocation_frame);
    }
    stack.back() = allocation_index;
}

// Python public APIs

bool
RecordReader::nextAllocationRecord(Allocation* allocation)
{
    while (true) {
        RecordType record_type;
        if (!d_input->read(reinterpret_cast<char*>(&record_type), sizeof(RecordType))) {
            return false;
        }

        switch (record_type) {
            case RecordType::ALLOCATION: {
                AllocationRecord record{};
                if (!parseAllocationRecord(record)) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to parse allocation record";
                    return false;
                }
                size_t f_index = getAllocationFrameIndex(record);
                *allocation = Allocation{
                        .record = record,
                        .frame_index = f_index,
                        .native_segment_generation = d_symbol_resolver.currentSegmentGeneration()};
                return true;
            }
            case RecordType::FRAME_PUSH:
                if (!parseFramePush()) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to parse frame push";
                    return false;
                }
                break;
            case RecordType::FRAME_POP:
                if (!parseFramePop()) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to parse frame pop";
                    return false;
                }
                break;
            case RecordType::FRAME_INDEX:
                if (!parseFrameIndex()) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to parse frame index";
                    return false;
                }
                break;
            case RecordType::NATIVE_TRACE_INDEX:
                if (!parseNativeFrameIndex()) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to parse native frame index";
                    return false;
                }
                break;
            case RecordType::MEMORY_MAP_START: {
                std::lock_guard<std::mutex> lock(d_mutex);
                d_symbol_resolver.clearSegments();
                break;
            }
            case RecordType::SEGMENT_HEADER:
                if (!parseSegmentHeader()) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to parse segment header";
                    return false;
                }
                break;
            case RecordType::THREAD_RECORD: {
                if (!parseThreadRecord()) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to parse thread record";
                    return false;
                }
                break;
            }
            default:
                if (d_input->is_open()) LOG(ERROR) << "Invalid record type";
                return false;
        }
    }
}

PyObject*
RecordReader::Py_GetStackFrame(unsigned int index, size_t max_stacks)
{
    std::lock_guard<std::mutex> lock(d_mutex);

    size_t stacks_obtained = 0;
    FrameTree::index_t current_index = index;
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        return nullptr;
    }

    int current_lineno = -1;
    while (current_index != 0 && stacks_obtained++ != max_stacks) {
        auto node = d_tree.nextNode(current_index);
        const auto& frame = d_frame_map.at(node.frame_id);
        PyObject* pyframe = frame.toPythonObject(d_pystring_cache, current_lineno);
        if (pyframe == nullptr) {
            goto error;
        }
        int ret = PyList_Append(list, pyframe);
        Py_DECREF(pyframe);
        if (ret != 0) {
            goto error;
        }
        current_index = node.parent_index;
        current_lineno = frame.parent_lineno;
    }
    return list;
error:
    Py_XDECREF(list);
    return nullptr;
}

PyObject*
RecordReader::Py_GetNativeStackFrame(FrameTree::index_t index, size_t generation, size_t max_stacks)
{
    std::lock_guard<std::mutex> lock(d_mutex);

    size_t stacks_obtained = 0;
    FrameTree::index_t current_index = index;
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        return nullptr;
    }

    while (current_index != 0 && stacks_obtained++ != max_stacks) {
        auto frame = d_native_frames[current_index - 1];
        current_index = frame.index;
        auto resolved_frames = d_symbol_resolver.resolve(frame.ip, generation);
        if (!resolved_frames) {
            continue;
        }
        for (auto& native_frame : resolved_frames->frames()) {
            PyObject* pyframe = native_frame.toPythonObject(d_pystring_cache);
            if (pyframe == nullptr) {
                return nullptr;
            }
            int ret = PyList_Append(list, pyframe);
            Py_DECREF(pyframe);
            if (ret != 0) {
                goto error;
            }
        }
    }
    return list;
error:
    Py_XDECREF(list);
    return nullptr;
}

HeaderRecord
RecordReader::getHeader() const noexcept
{
    return d_header;
}

std::string
RecordReader::getThreadName(thread_id_t tid)
{
    auto it = d_thread_names.find(tid);
    if (it != d_thread_names.end()) {
        return it->second;
    }
    return "";
}

PyObject*
RecordReader::dumpAllRecords()
{
    printf("HEADER magic=%.*s version=%d native_traces=%s"
           " n_allocations=%zd n_frames=%zd start_time=%lld end_time=%lld"
           " pid=%d command_line=%s\n",
           (int)sizeof(d_header.magic),
           d_header.magic,
           d_header.version,
           d_header.native_traces ? "true" : "false",
           d_header.stats.n_allocations,
           d_header.stats.n_frames,
           d_header.stats.start_time,
           d_header.stats.end_time,
           d_header.pid,
           d_header.command_line.c_str());

    while (true) {
        if (0 != PyErr_CheckSignals()) {
            return NULL;
        }

        RecordType record_type;
        if (!d_input->read(reinterpret_cast<char*>(&record_type), sizeof(RecordType))) {
            Py_RETURN_NONE;
        }

        switch (record_type) {
            case RecordType::ALLOCATION: {
                printf("ALLOCATION ");

                AllocationRecord record;
                if (!d_input->read(reinterpret_cast<char*>(&record), sizeof(record))) {
                    Py_RETURN_NONE;
                }

                const char* allocator = allocatorName(record.allocator);

                std::string unknownAllocator;
                if (!allocator) {
                    unknownAllocator =
                            "<unknown allocator " + std::to_string((int)record.allocator) + ">";
                    allocator = unknownAllocator.c_str();
                }

                printf("tid=%lu address=%p size=%zd allocator=%s"
                       " py_lineno=%d native_frame_id=%zd\n",
                       record.tid,
                       (void*)record.address,
                       record.size,
                       allocator,
                       record.py_lineno,
                       record.native_frame_id);
            } break;
            case RecordType::FRAME_PUSH: {
                printf("FRAME_PUSH ");

                FramePush record;
                if (!d_input->read(reinterpret_cast<char*>(&record), sizeof(record))) {
                    Py_RETURN_NONE;
                }

                printf("tid=%lu frame_id=%zd\n", record.tid, record.frame_id);
            } break;
            case RecordType::FRAME_POP: {
                printf("FRAME_POP ");

                FramePop record;
                if (!d_input->read(reinterpret_cast<char*>(&record), sizeof(record))) {
                    Py_RETURN_NONE;
                }

                printf("tid=%lu count=%u\n", record.tid, record.count);
            } break;
            case RecordType::FRAME_INDEX: {
                printf("FRAME_ID ");

                tracking_api::pyframe_map_val_t record;
                if (!d_input->read(reinterpret_cast<char*>(&record.first), sizeof(record.first))
                    || !d_input->getline(record.second.function_name, '\0')
                    || !d_input->getline(record.second.filename, '\0')
                    || !d_input->read(
                            reinterpret_cast<char*>(&record.second.parent_lineno),
                            sizeof(record.second.parent_lineno)))
                {
                    Py_RETURN_NONE;
                }

                printf("frame_id=%zd function_name=%s filename=%s parent_lineno=%d\n",
                       record.first,
                       record.second.function_name.c_str(),
                       record.second.filename.c_str(),
                       record.second.parent_lineno);
            } break;
            case RecordType::NATIVE_TRACE_INDEX: {
                printf("NATIVE_FRAME_ID ");

                UnresolvedNativeFrame record;
                if (!d_input->read(reinterpret_cast<char*>(&record), sizeof(record))) {
                    Py_RETURN_NONE;
                }

                printf("ip=%p index=%u\n", (void*)record.ip, record.index);
            } break;
            case RecordType::MEMORY_MAP_START: {
                printf("MEMORY_MAP_START\n");
            } break;
            case RecordType::SEGMENT_HEADER: {
                printf("SEGMENT_HEADER ");

                std::string filename;
                size_t num_segments;
                uintptr_t addr;
                if (!d_input->getline(filename, '\0')
                    || !d_input->read(reinterpret_cast<char*>(&num_segments), sizeof(num_segments))
                    || !d_input->read(reinterpret_cast<char*>(&addr), sizeof(addr)))
                {
                    Py_RETURN_NONE;
                }

                printf("filename=%s num_segments=%zd addr=%p\n",
                       filename.c_str(),
                       num_segments,
                       (void*)addr);
            } break;
            case RecordType::SEGMENT: {
                printf("SEGMENT ");

                Segment record;
                if (!d_input->read(reinterpret_cast<char*>(&record), sizeof(record))) {
                    Py_RETURN_NONE;
                }

                printf("%p %lu\n", (void*)record.vaddr, record.memsz);
            } break;
            case RecordType::THREAD_RECORD: {
                printf("THREAD ");

                thread_id_t tid;
                std::string name;
                if (!d_input->read(reinterpret_cast<char*>(&tid), sizeof(thread_id_t))
                    || !d_input->getline(name, '\0')) {
                    Py_RETURN_NONE;
                }

                printf("%ld %s\n", tid, name.c_str());
            } break;
            default: {
                printf("UNKNOWN RECORD TYPE %d\n", (int)record_type);
                Py_RETURN_NONE;
            } break;
        }
    }
}

}  // namespace pensieve::api
