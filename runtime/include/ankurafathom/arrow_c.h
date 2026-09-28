// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

/// \file abi.h Arrow C Data Interface
///
/// The Arrow C Data interface defines a very small, stable set
/// of C definitions which can be easily copied into any project's
/// source code and vendored to be used for columnar data interchange
/// in the Arrow format. For non-C/C++ languages and runtimes,
/// it should be almost as easy to translate the C definitions into
/// the corresponding C FFI declarations.
///
/// Applications and libraries can therefore work with Arrow memory
/// without necessarily using the Arrow libraries or reinventing
/// the wheel. Developers can choose between tight integration
/// with the Arrow software project or minimal integration with
/// the Arrow format only.

#pragma once

#include <stdint.h>

// Spec and documentation: https://arrow.apache.org/docs/format/CDataInterface.html

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ARROW_C_DATA_INTERFACE
#  define ARROW_C_DATA_INTERFACE

#  define ARROW_FLAG_DICTIONARY_ORDERED 1
#  define ARROW_FLAG_NULLABLE 2
#  define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema {
  // Array type description
  const char* format;
  const char* name;
  const char* metadata;
  int64_t flags;
  int64_t n_children;
  struct ArrowSchema** children;
  struct ArrowSchema* dictionary;

  // Release callback
  void (*release)(struct ArrowSchema*);
  // Opaque producer-specific data
  void* private_data;
};

struct ArrowArray {
  // Array data description
  int64_t length;
  int64_t null_count;
  int64_t offset;
  int64_t n_buffers;
  int64_t n_children;
  const void** buffers;
  struct ArrowArray** children;
  struct ArrowArray* dictionary;

  // Release callback
  void (*release)(struct ArrowArray*);
  // Opaque producer-specific data
  void* private_data;
};

#  define ARROW_STATISTICS_KEY_AVERAGE_BYTE_WIDTH_EXACT "ARROW:average_byte_width:exact"
#  define ARROW_STATISTICS_KEY_AVERAGE_BYTE_WIDTH_APPROXIMATE \
    "ARROW:average_byte_width:approximate"
#  define ARROW_STATISTICS_KEY_DISTINCT_COUNT_EXACT "ARROW:distinct_count:exact"
#  define ARROW_STATISTICS_KEY_DISTINCT_COUNT_APPROXIMATE \
    "ARROW:distinct_count:approximate"
#  define ARROW_STATISTICS_KEY_MAX_BYTE_WIDTH_EXACT "ARROW:max_byte_width:exact"
#  define ARROW_STATISTICS_KEY_MAX_BYTE_WIDTH_APPROXIMATE \
    "ARROW:max_byte_width:approximate"
#  define ARROW_STATISTICS_KEY_MAX_VALUE_EXACT "ARROW:max_value:exact"
#  define ARROW_STATISTICS_KEY_MAX_VALUE_APPROXIMATE "ARROW:max_value:approximate"
#  define ARROW_STATISTICS_KEY_MIN_VALUE_EXACT "ARROW:min_value:exact"
#  define ARROW_STATISTICS_KEY_MIN_VALUE_APPROXIMATE "ARROW:min_value:approximate"
#  define ARROW_STATISTICS_KEY_NULL_COUNT_EXACT "ARROW:null_count:exact"
#  define ARROW_STATISTICS_KEY_NULL_COUNT_APPROXIMATE "ARROW:null_count:approximate"
#  define ARROW_STATISTICS_KEY_ROW_COUNT_EXACT "ARROW:row_count:exact"
#  define ARROW_STATISTICS_KEY_ROW_COUNT_APPROXIMATE "ARROW:row_count:approximate"

#endif  // ARROW_C_DATA_INTERFACE

// CPU data/stream subset of Apache Arrow 25.0.1 arrow/c/abi.h.
// Device/async definitions omitted; see third_party/arrow_c/README.md.

#ifndef ARROW_C_STREAM_INTERFACE
#  define ARROW_C_STREAM_INTERFACE

struct ArrowArrayStream {
  // Callback to get the stream type
  // (will be the same for all arrays in the stream).
  //
  // Return value: 0 if successful, an `errno`-compatible error code otherwise.
  //
  // If successful, the ArrowSchema must be released independently from the stream.
  int (*get_schema)(struct ArrowArrayStream*, struct ArrowSchema* out);

  // Callback to get the next array
  // (if no error and the array is released, the stream has ended)
  //
  // Return value: 0 if successful, an `errno`-compatible error code otherwise.
  //
  // If successful, the ArrowArray must be released independently from the stream.
  int (*get_next)(struct ArrowArrayStream*, struct ArrowArray* out);

  // Callback to get optional detailed error information.
  // This must only be called if the last stream operation failed
  // with a non-0 return code.
  //
  // Return value: pointer to a null-terminated character array describing
  // the last error, or NULL if no description is available.
  //
  // The returned pointer is only valid until the next operation on this stream
  // (including release).
  const char* (*get_last_error)(struct ArrowArrayStream*);

  // Release callback: release the stream's own resources.
  // Note that arrays returned by `get_next` must be individually released.
  void (*release)(struct ArrowArrayStream*);

  // Opaque producer-specific data
  void* private_data;
};

#endif  // ARROW_C_STREAM_INTERFACE

#ifdef __cplusplus
}
#endif
