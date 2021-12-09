import logging
import os
import pathlib
import sys

cimport cython

import threading
from datetime import datetime

from posix.mman cimport MAP_ANONYMOUS
from posix.mman cimport MAP_FAILED
from posix.mman cimport MAP_SHARED
from posix.mman cimport PROT_WRITE
from posix.mman cimport mmap
from posix.mman cimport munmap

from _pensieve.alloc cimport calloc
from _pensieve.alloc cimport free
from _pensieve.alloc cimport malloc
from _pensieve.alloc cimport memalign
from _pensieve.alloc cimport posix_memalign
from _pensieve.alloc cimport pvalloc
from _pensieve.alloc cimport realloc
from _pensieve.alloc cimport valloc
from _pensieve.logging cimport initializePythonLoggerInterface
from _pensieve.pthread cimport pthread_create
from _pensieve.pthread cimport pthread_join
from _pensieve.pthread cimport pthread_t
from _pensieve.record_reader cimport RecordReader
from _pensieve.record_writer cimport RecordWriter
from _pensieve.records cimport Allocation as NativeAllocation
from _pensieve.sink cimport FileSink
from _pensieve.sink cimport Sink
from _pensieve.sink cimport SocketSink
from _pensieve.snapshot cimport HighWatermark
from _pensieve.snapshot cimport Py_GetSnapshotAllocationRecords
from _pensieve.snapshot cimport getHighWatermark
from _pensieve.socket_reader_thread cimport BackgroundSocketReader
from _pensieve.source cimport FileSource
from _pensieve.source cimport SocketSource
from _pensieve.tracking_api cimport Tracker as NativeTracker
from _pensieve.tracking_api cimport install_trace_function
from libc.errno cimport errno
from libc.stdint cimport uint16_t
from libc.stdint cimport uintptr_t
from libcpp cimport bool
from libcpp.memory cimport make_shared
from libcpp.memory cimport make_unique
from libcpp.memory cimport shared_ptr
from libcpp.memory cimport unique_ptr
from libcpp.string cimport string as cppstring
from libcpp.utility cimport move
from libcpp.vector cimport vector

import typing

from ._destination import FileDestination
from ._destination import SocketDestination
from ._metadata import Metadata

initializePythonLoggerInterface()

LOGGER = logging.getLogger(__file__)

cdef unique_ptr[NativeTracker] _TRACKER

# Testing utilities
# This code is at the top so that tests which rely on line numbers don't have to
# be updated every time a line change is introduced in the core pensieve code.

cdef class MemoryAllocator:
    cdef void* ptr

    def __cinit__(self):
        self.ptr = NULL

    @cython.profile(True)
    def free(self):
        if self.ptr == NULL:
            raise RuntimeError("Pointer cannot be NULL")
        free(self.ptr)
        self.ptr = NULL

    @cython.profile(True)
    def malloc(self, size_t size):
        self.ptr = malloc(size)

    @cython.profile(True)
    def calloc(self, size_t size):
        self.ptr = calloc(1, size)

    @cython.profile(True)
    def realloc(self, size_t size):
        self.ptr = malloc(1)
        self.ptr = realloc(self.ptr, size)

    @cython.profile(True)
    def posix_memalign(self, size_t size):
        posix_memalign(&self.ptr, sizeof(void*), size)

    @cython.profile(True)
    def memalign(self, size_t size):
        self.ptr = memalign(sizeof(void*), size)

    @cython.profile(True)
    def valloc(self, size_t size):
        self.ptr = valloc(size)

    @cython.profile(True)
    def pvalloc(self, size_t size):
        self.ptr = pvalloc(size)

    @cython.profile(True)
    def run_in_pthread(self, callback):
        cdef pthread_t thread
        cdef int ret = pthread_create(&thread, NULL, &_pthread_worker, <void*>callback)
        if ret != 0:
            raise RuntimeError("Failed to create thread")
        with nogil:
            pthread_join(thread, NULL)


@cython.profile(True)
def _cython_nested_allocation(allocator_fn, size):
    allocator_fn(size)
    cdef void* p = valloc(size);
    free(p)

cdef class MmapAllocator:
    cdef uintptr_t _address

    @cython.profile(True)
    def __cinit__(self, size, address=0):
        cdef uintptr_t start_address = address

        self._address = <uintptr_t>mmap(<void *>start_address, size, PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)
        if <void *>self._address == MAP_FAILED:
            raise MemoryError

    @property
    def address(self):
        return self._address

    @cython.profile(True)
    def munmap(self, length, offset=0):
        cdef uintptr_t addr = self._address + <uintptr_t> offset
        cdef int ret = munmap(<void *>addr, length)
        if ret != 0:
            raise MemoryError(f"munmap rcode: {ret} errno: {errno}")

@cython.profile(True)
cdef void* _pthread_worker(void* arg) with gil:
    (<object> arg)()

cdef api void log_with_python(cppstring message, int level) except*:
    LOGGER.log(level, message)


cpdef enum AllocatorType:
    MALLOC = 1
    FREE = 2
    CALLOC = 3
    REALLOC = 4
    POSIX_MEMALIGN = 5
    MEMALIGN = 6
    VALLOC = 7
    PVALLOC = 8
    MMAP = 9
    MUNMAP = 10


def size_fmt(num, suffix='B'):
    for unit in ['','K','M','G','T','P','E','Z']:
        if abs(num) < 1024.0:
            return f"{num:5.3f}{unit}{suffix}"
        num /= 1024.0
    return f"{num:.1f}Y{suffix}"

# Pensieve core

PYTHON_VERSION = (sys.version_info.major, sys.version_info.minor)

@cython.freelist(1024)
cdef class AllocationRecord:
    cdef object _tuple
    cdef object _stack_trace
    cdef object _native_stack_trace
    cdef shared_ptr[RecordReader] _reader

    def __init__(self, record):
        self._tuple = record
        self._stack_trace = None

    def __eq__(self, other):
        cdef AllocationRecord _other
        if isinstance(other, AllocationRecord):
            _other = other
            return self._tuple == _other._tuple
        return NotImplemented

    def __hash__(self):
        return hash(self._tuple)

    @property
    def tid(self):
        return self._tuple[0]

    @property
    def address(self):
        return self._tuple[1]

    @property
    def size(self):
        return self._tuple[2]

    @property
    def allocator(self):
        return self._tuple[3]

    @property
    def stack_id(self):
        return self._tuple[4]

    @property
    def n_allocations(self):
        return self._tuple[5]

    def stack_trace(self, max_stacks=None):
        assert self._reader.get() != NULL, "Cannot get stack trace without reader."
        if self._stack_trace is None:
            if max_stacks is None:
                self._stack_trace = self._reader.get().Py_GetStackFrame(self._tuple[4])
            else:
                self._stack_trace = self._reader.get().Py_GetStackFrame(self._tuple[4], max_stacks)
        return self._stack_trace

    def native_stack_trace(self, max_stacks=None):
        assert self._reader.get() != NULL, "Cannot get stack trace without reader."
        if self._native_stack_trace is None:
            if max_stacks is None:
                self._native_stack_trace = self._reader.get().Py_GetNativeStackFrame(
                        self._tuple[6], self._tuple[7])
            else:
                self._native_stack_trace = self._reader.get().Py_GetNativeStackFrame(
                        self._tuple[6], self._tuple[7], max_stacks)
        return self._native_stack_trace

    cdef _is_eval_frame(self, object symbol):
        return "_PyEval_EvalFrameDefault" in symbol

    def _pure_python_stack_trace(self, max_stacks):
        for frame in self.stack_trace(max_stacks):
            _, file, _ = frame
            if file.endswith(".pyx"):
                continue
            yield frame

    def hybrid_stack_trace(self, max_stacks=None):
        python_stack = tuple(self._pure_python_stack_trace(max_stacks))
        n_python_frames_left = len(python_stack) if python_stack else None
        python_stack = iter(python_stack)
        for native_frame in self.native_stack_trace(max_stacks):
            if n_python_frames_left == 0:
                break
            symbol, *_ = native_frame
            if self._is_eval_frame(symbol):
                python_frame =  next(python_stack)
                n_python_frames_left -= 1
                yield python_frame
            else:
                yield native_frame

    def __repr__(self):
        return (f"AllocationRecord<tid={hex(self.tid)}, address={hex(self.address)}, "
                f"size={'N/A' if not self.size else size_fmt(self.size)}, allocator={self.allocator!r}, "
                f"allocations={self.n_allocations}>")


cdef class Tracker:
    cdef bool _native_traces
    cdef object _previous_profile_func
    cdef object _previous_thread_profile_func
    cdef shared_ptr[RecordReader] _reader
    cdef unique_ptr[RecordWriter] _writer

    cdef unique_ptr[Sink] _make_writer(self, destination) except*:
        # Creating a Sink can raise Python exceptions (if is interrupted by signal
        # handlers). If this happens, this method will propagate the appropriate exception.
        if isinstance(destination, FileDestination):
            return unique_ptr[Sink](new FileSink(os.fsencode(destination.path)))

        elif isinstance(destination, SocketDestination):
            return unique_ptr[Sink](new SocketSink(destination.host, destination.port))
        else:
            raise TypeError("destination must be a SocketDestination or FileDestination")


    def __cinit__(self, object file_name=None, *, object destination=None, bool native_traces=False):
        if (file_name, destination).count(None) != 1:
            raise TypeError("Exactly one of 'file_name' or 'destination' argument must be specified")

        cdef cppstring command_line = " ".join(sys.argv)
        self._native_traces = native_traces

        if file_name is not None:
            destination = FileDestination(path=file_name)

        self._writer = make_unique[RecordWriter](
                move(self._make_writer(destination)), command_line, native_traces
            )

    @cython.profile(False)
    def __enter__(self):

        if _TRACKER.get() != NULL:
            raise RuntimeError("No more than one Tracker instance can be active at the same time")

        cdef unique_ptr[RecordWriter] writer
        if self._writer == NULL:
            raise RuntimeError("Attempting to use stale output handle")
        writer = move(self._writer)

        self._previous_profile_func = sys.getprofile()
        self._previous_thread_profile_func = threading._profile_hook
        threading.setprofile(start_thread_trace)

        _TRACKER.reset(new NativeTracker(move(writer), self._native_traces))
        return self

    def __del__(self):
        self._reader.reset()

    @cython.profile(False)
    def __exit__(self, exc_type, exc_value, exc_traceback):
        _TRACKER.reset(NULL)
        sys.setprofile(self._previous_profile_func)
        threading.setprofile(self._previous_thread_profile_func)


def start_thread_trace(frame, event, arg):
    if event in {"call", "c_call"}:
        install_trace_function()
    return start_thread_trace


cdef class FileReader:
    cdef cppstring _path

    cdef shared_ptr[RecordReader] _reader
    cdef vector[NativeAllocation] _native_allocations
    cdef unique_ptr[HighWatermark] _high_watermark
    cdef bool _closed
    cdef object _header

    def __init__(self, object file_name):
        self._path = str(file_name)
        if not pathlib.Path(self._path).exists():
            raise IOError(f"No such file: {self._path}")
        self._reader = make_shared[RecordReader](unique_ptr[FileSource](new FileSource(self._path)))
        self._header: dict = self._reader.get().getHeader()

    cdef RecordReader* _get_reader(self) except *:
        if self._reader.get() == NULL:
            raise ValueError("Operation on a closed FileReader")
        return self._reader.get()

    def close(self):
        cdef RecordReader* reader = self._get_reader()
        reader.close()

    cdef void _ensure_reader_is_open(self) except *:
        if not self._get_reader().isOpen():
            raise ValueError("Operation on a closed FileReader")

    @property
    def closed(self):
        return not self._get_reader().isOpen()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, exc_traceback):
        self.close()

    def __dealloc__(self):
        self._reader.reset()

    cdef inline void _populate_allocations(self) except*:
        if self._native_allocations.size() != 0:
            return

        cdef RecordReader * reader = self._get_reader()
        cdef NativeAllocation native_allocation
        total_allocations = self._header["stats"]["n_allocations"]
        self._native_allocations.reserve(total_allocations)
        while reader.nextAllocationRecord(&native_allocation):
            self._native_allocations.push_back(move(native_allocation))

    def _yield_allocations(self, size_t index, merge_threads):
        for elem in Py_GetSnapshotAllocationRecords(self._native_allocations, index, merge_threads):
            alloc = AllocationRecord(elem)
            (<AllocationRecord> alloc)._reader = self._reader
            yield alloc
            self._ensure_reader_is_open()

    cdef inline HighWatermark* _get_high_watermark(self) except*:
        if self._high_watermark == NULL:
            self._populate_allocations()
            self._high_watermark = make_unique[HighWatermark](getHighWatermark(self._native_allocations))
        return self._high_watermark.get()

    def get_high_watermark_allocation_records(self, merge_threads=True):
        self._ensure_reader_is_open()
        self._populate_allocations()
        cdef HighWatermark* watermark = self._get_high_watermark()
        yield from self._yield_allocations(watermark.index, merge_threads)

    def get_leaked_allocation_records(self, merge_threads=True):
        self._ensure_reader_is_open()
        self._populate_allocations()
        cdef size_t snapshot_index = self._native_allocations.size() - 1
        yield from self._yield_allocations(snapshot_index, merge_threads)

    def get_allocation_records(self):
        cdef shared_ptr[RecordReader] reader = make_shared[RecordReader](
            unique_ptr[FileSource](new FileSource(self._path))
        )
        cdef NativeAllocation native_allocation
        cdef RecordReader* reader_ptr = reader.get()

        while reader_ptr.nextAllocationRecord(&native_allocation):
            alloc = AllocationRecord(native_allocation.toPythonObject())
            (<AllocationRecord> alloc)._reader = reader
            yield alloc
        reader_ptr.close()

    @property
    def metadata(self):
        def millis_to_dt(millis) -> datetime:
            return datetime.fromtimestamp(millis // 1000).replace(
                microsecond=millis % 1000 * 1000)

        stats = self._header["stats"]
        return Metadata(start_time=millis_to_dt(stats["start_time"]),
                        end_time=millis_to_dt(stats["end_time"]),
                        total_allocations=stats["n_allocations"],
                        total_frames=stats["n_frames"],
                        peak_memory=self._get_high_watermark().peak_memory,
                        command_line=self._header["command_line"],
                        pid=self._header["pid"])

    @property
    def has_native_traces(self):
        return self._header["native_traces"]


cdef class SocketReader:
    cdef BackgroundSocketReader* _impl
    cdef shared_ptr[RecordReader] _reader
    cdef object _header
    cdef object _port

    def __cinit__(self, int port):
        self._impl = NULL

    def __init__(self, port: int):
        self._header = {}
        self._port = port

    cdef _teardown(self):
        with nogil:
            del self._impl
        self._impl = NULL

    cdef unique_ptr[SocketSource] _make_source(self) except*:
        # Creating a SocketSource can raise Python exceptions (if is interrupted by signal
        # handlers). If this happens, this method will propagate the appropriate exception.
        # We cannot use make_unique or C++ exceptions from SocketSource() won't be caught.
        cdef SocketSource* source = new SocketSource(self._port)
        return unique_ptr[SocketSource](source)

    def __enter__(self):
        if self._impl is not NULL:
            raise ValueError(
                "Can not enter the context of a SocketReader object more than "
                "once, at the same time."
            )

        self._reader = make_shared[RecordReader](move(self._make_source()))
        self._header = self._reader.get().getHeader()

        self._impl = new BackgroundSocketReader(self._reader)
        self._impl.start()

        return self

    def __exit__(self, exc_type, exc_value, exc_traceback):
        assert self._impl is not NULL

        self._teardown()
        self._reader.get().close()

    def __dealloc__(self):
        if self._impl is not NULL:
            self._teardown()

    @property
    def command_line(self):
        if not self._header:
            return None
        return self._header["command_line"]

    @property
    def is_active(self):
        return self._impl.is_active()

    @property
    def pid(self):
        if not self._header:
            return None
        return self._header["pid"]

    def get_current_snapshot(self, *, bool merge_threads):
        if self._impl is NULL:
            raise ValueError("No active thread to get snapshot from.")

        snapshot_allocations = self._impl.Py_GetSnapshotAllocationRecords(merge_threads=merge_threads)
        for elem in snapshot_allocations:
            alloc = AllocationRecord(elem)
            (<AllocationRecord> alloc)._reader = self._reader
            yield alloc
