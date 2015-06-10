/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include "fstream.hh"
#include "align.hh"
#include <malloc.h>
#include <string.h>

class file_data_source_impl : public data_source_impl {
    lw_shared_ptr<file> _file;
    uint64_t _pos;
    std::experimental::optional<size_t> _fsize;
    size_t _buffer_size;
private:
    // Should be called only when _fsize is initialized
    future<temporary_buffer<char>> do_get() {
        using buf_type = temporary_buffer<char>;
        size_t read_size = _buffer_size;
        size_t fsize = _fsize.value();

        if (_pos >= fsize) {
            return make_ready_future<buf_type>(std::move(buf_type(0)));
        } else if (_pos + _buffer_size > fsize) {
            read_size = fsize - _pos;
        }

        return _file->dma_read_bulk<char>(_pos, read_size).then(
                [this] (buf_type buf) {
            _pos += buf.size();

            return std::move(buf);
        });
    }
public:
    file_data_source_impl(lw_shared_ptr<file> f, uint64_t pos, size_t buffer_size)
            : _file(std::move(f)), _pos(pos), _buffer_size(buffer_size) {}

    virtual future<temporary_buffer<char>> get() override {
        if (!_fsize){
            return _file->size().then(
                    [this] (size_t fsize) {
                _fsize = fsize;

                return do_get();
            });
        }

        return do_get();
    }
};

class file_data_source : public data_source {
public:
    file_data_source(lw_shared_ptr<file> f, uint64_t offset, size_t buffer_size)
        : data_source(std::make_unique<file_data_source_impl>(
                std::move(f), offset, buffer_size)) {}
};

input_stream<char> make_file_input_stream(
        lw_shared_ptr<file> f, uint64_t offset, size_t buffer_size) {
    return input_stream<char>(file_data_source(std::move(f), offset, buffer_size));
}

class file_data_sink_impl : public data_sink_impl {
    lw_shared_ptr<file> _file;
    size_t _buffer_size;
    uint64_t _pos = 0;
public:
    file_data_sink_impl(lw_shared_ptr<file> f, size_t buffer_size)
            : _file(std::move(f)), _buffer_size(buffer_size) {}
    future<> put(net::packet data) { return make_ready_future<>(); }
    virtual temporary_buffer<char> allocate_buffer(size_t size) override {
        // buffers to dma_write must be aligned to 512 bytes.
        return temporary_buffer<char>::aligned(512, size);
    }
    virtual future<> put(temporary_buffer<char> buf) override {
        bool truncate = false;
        auto pos = _pos;
        _pos += buf.size();
        auto p = static_cast<const char*>(buf.get());
        size_t buf_size = buf.size();

        if ((buf.size() & 511) != 0) {
            // If buf size isn't aligned, copy its content into a new aligned buf.
            // This should only happen when the user calls output_stream::flush().
            auto tmp = allocate_buffer(align_up(buf.size(), 512UL));
            ::memcpy(tmp.get_write(), buf.get(), buf.size());
            buf = std::move(tmp);
            p = buf.get();
            buf_size = buf.size();
            truncate = true;
        }
        return _file->dma_write(pos, p, buf_size).then(
            [this, buf = std::move(buf), truncate] (size_t size) {
            if (truncate) {
                return _file->truncate(_pos).then([this] {
                    return _file->flush();
                });
            }
            return make_ready_future<>();
        });
    }
    future<> close() { return _file->flush(); }
};

class file_data_sink : public data_sink {
public:
    file_data_sink(lw_shared_ptr<file> f, size_t buffer_size)
        : data_sink(std::make_unique<file_data_sink_impl>(
                std::move(f), buffer_size)) {}
};

output_stream<char> make_file_output_stream(lw_shared_ptr<file> f, size_t buffer_size) {
    return output_stream<char>(file_data_sink(std::move(f), buffer_size), buffer_size);
}
