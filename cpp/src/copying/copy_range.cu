/*
 * Copyright (c) 2019, NVIDIA CORPORATION.
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

#include "copy_range.cuh"
#include <cudf/copying.hpp>

#include "../../tests/utilities/cudf_test_fixtures.h"

namespace cudf {

namespace detail {

struct scalar_factory {
  gdf_scalar value;

  template <typename T>
  struct scalar_functor {
    T value;
    bool is_valid;

    __device__
    T data(gdf_index_type index) { return value; }

    __device__
    bool valid(gdf_index_type index) { return is_valid; }
  };

  template <typename T>
  scalar_functor<T> operator()() {
    T val{}; // Safe type pun, compiler should optimize away the memcpy
    memcpy(&val, &value.data, sizeof(T));
    return scalar_functor<T>{val, value.is_valid};
  }
};

struct column_range_factory {
  gdf_column column;
  gdf_index_type begin;

  template <typename T>
  struct column_range_functor {
    T const * column_data;
    bit_mask_t const * bitmask;
    gdf_index_type begin;

    __device__
    T data(gdf_index_type index) { 
      return column_data[begin + index]; }

    __device__
    bool valid(gdf_index_type index) {
      return bit_mask::is_valid(bitmask, begin + index);
    }
  };

  template <typename T>
  column_range_functor<T> operator()() {
    return column_range_functor<T>{
      static_cast<T*>(column.data),
      reinterpret_cast<bit_mask_t*>(column.valid),
      begin
    };
  }
};

}; // namespace detail

void copy_range(gdf_column *out_column, gdf_column const &in_column,
                gdf_index_type out_begin, gdf_index_type out_end, 
                gdf_index_type in_begin)
{
  gdf_size_type num_elements = out_end - out_begin;
  if (num_elements != 0) { // otherwise no-op
    validate(in_column);
    validate(out_column);
    CUDF_EXPECTS(out_column->dtype == in_column.dtype, "Data type mismatch");
    // out range validated by detail::copy_range
    CUDF_EXPECTS((in_begin >= 0) && (in_begin + num_elements <= in_column.size),
                 "Range is out of bounds");

    if (out_column->dtype == GDF_STRING_CATEGORY) {
      // if the columns are string types then we need to combine categories
      // before copying to ensure the strings referred to by the new indices
      // are included in the destination column

      gdf_column * input_cols[2] = {out_column,
                                    const_cast<gdf_column*>(&in_column)};

      // make temporary columns which will have synced categories
      // TODO: these copies seem excessively expensive, but 
      // sync_column_categories doesn't copy the valid mask
      gdf_column temp_out = cudf::copy(*out_column);
      gdf_column temp_in  = cudf::copy(in_column);
      gdf_column * temp_cols[2] = {&temp_out, &temp_in};

      // sync categories
      CUDF_EXPECTS(GDF_SUCCESS ==
        sync_column_categories(input_cols, temp_cols, 2),
        "Failed to synchronize NVCategory");

      detail::copy_range(&temp_out,
                         detail::column_range_factory{temp_in, in_begin},
                         out_begin, out_end);

      std::swap(out_column->data, temp_out.data);
      std::swap(out_column->valid, temp_out.valid);
      std::swap(out_column->null_count, temp_out.null_count);
      std::swap(out_column->dtype_info.category, temp_out.dtype_info.category);
      
      gdf_column_free(&temp_out);
      gdf_column_free(&temp_in);
    }
    else {
      detail::copy_range(out_column,
                         detail::column_range_factory{in_column, in_begin},
                         out_begin, out_end);
    }
  }
}

void fill(gdf_column *column, gdf_scalar const& value, 
          gdf_index_type begin, gdf_index_type end)
{ 
  if (end != begin) { // otherwise no-op
    validate(column);
    CUDF_EXPECTS(column->dtype == value.dtype, "Data type mismatch");
    detail::copy_range(column, detail::scalar_factory{value}, begin, end);
  }
}

}; // namespace cudf