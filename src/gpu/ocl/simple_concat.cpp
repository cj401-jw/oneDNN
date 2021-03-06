/*******************************************************************************
* Copyright 2020 Intel Corporation
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
*******************************************************************************/

#include "gpu/ocl/simple_concat.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {
static status_t init_conf_common(concat_conf_t &conf, const concat_pd_t *pd) {
    using namespace utils;

    const memory_desc_wrapper dst_mdw(pd->dst_md());
    auto ndims = dst_mdw.ndims();
    auto nelems = dst_mdw.nelems(true);
    auto data_type_size = dst_mdw.data_type_size();

    if (nelems == 0) return status::unimplemented;

    const auto &blk = dst_mdw.blocking_desc();

    int pre_concat_dim = 0;
    bool is_first = true;
    for (int i = 0; i < ndims; ++i) {
        if (blk.strides[i] <= blk.strides[pre_concat_dim]
                && blk.strides[i] > blk.strides[pd->concat_dim()]) {
            pre_concat_dim = i;
            is_first = false;
        }
    }
    int offset = 0;
    bool has_padding = false;
    for (int i = 0; i < pd->n_inputs(); ++i) {
        const memory_desc_wrapper src_mdw(pd->src_md(i));

        // check concat dim padding
        if (src_mdw.padded_dims()[pd->concat_dim()]
                != src_mdw.dims()[pd->concat_dim()]) {
            if (has_padding)
                return status::unimplemented;
            else
                has_padding = true;
        }

        if (src_mdw.data_type() != dst_mdw.data_type())
            return status::unimplemented;

        if (!types::blocking_desc_is_equal(*pd->dst_md(), *pd->src_md(i), true))
            return status::unimplemented;

        if (!src_mdw.is_dense()) return status::unimplemented;

        const auto &src_blk = src_mdw.blocking_desc();

        conf.offset[i] = offset;
        int src_extern_dim_size = (is_first)
                ? src_mdw.nelems(true)
                : (src_mdw.dims()[pd->concat_dim()] != 0)
                        * src_blk.strides[pre_concat_dim];
        offset += src_extern_dim_size / src_blk.strides[pd->concat_dim()];

        conf.src_extern_dim_sizes[i] = src_extern_dim_size * data_type_size / 4;
    }

    conf.dst_extern_dim_size
            = (is_first) ? nelems : blk.strides[pre_concat_dim];

    int extern_axis = nelems / conf.dst_extern_dim_size;
    int concat_dim_size
            = conf.dst_extern_dim_size / blk.strides[pd->concat_dim()];
    conf.inner_axis = blk.strides[pd->concat_dim()] * data_type_size;
    conf.n = pd->n_inputs();
    while (concat_dim_size % 2 == 0) {
        // check offsets
        bool ok = true;
        for (int i = 0; i < conf.n; ++i) {
            if (conf.offset[i] % 2) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
        for (int i = 0; i < conf.n; ++i) {
            conf.offset[i] /= 2;
        }
        concat_dim_size /= 2;
        conf.inner_axis *= 2;
    }
    if (conf.inner_axis % 4) return status::unimplemented;
    conf.inner_axis /= 4;
    conf.dst_extern_dim_size = conf.dst_extern_dim_size * data_type_size / 4;

    conf.block = (conf.inner_axis % 16 == 0) ? 16 : 1;
    if (conf.block == 16) {
        conf.block *= utils::max_div(conf.inner_axis / 16, 8);
    }

    int subgroup_size = (conf.block > 1) ? 16 : 1;

    conf.lws_d[0] = subgroup_size;
    conf.lws_d[1] = 1;
    conf.lws_d[2] = 1;

    conf.gws_d[0] = conf.inner_axis / conf.block * subgroup_size;
    conf.gws_d[1] = extern_axis;
    conf.gws_d[2] = concat_dim_size;
    conf.data_type = dst_mdw.data_type();

    return status::success;
}

static status_t init_kernel_ctx_common(
        compute::kernel_ctx_t &kernel_ctx, const concat_conf_t &conf) {
    kernel_ctx.define_int("DST_EXT_OFFSET", conf.dst_extern_dim_size);
    for (int i = 0; i < conf.n; ++i) {
        kernel_ctx.define_int(utils::format("SRC%d_EXT_OFFSET", i),
                conf.src_extern_dim_sizes[i]);
        kernel_ctx.define_int(utils::format("OFFSET%d", i), conf.offset[i]);
    }
    kernel_ctx.define_int(utils::format("OFFSET%d", conf.n), conf.gws_d[2]);
    kernel_ctx.define_int("INNER_OFFSET", conf.inner_axis);
    kernel_ctx.define_int("BLOCK", conf.block);
    kernel_ctx.define_int("N_INPUTS", conf.n);
    return status::success;
}

status_t simple_concat_t::pd_t::init_conf() {
    return init_conf_common(conf, this);
}

status_t simple_concat_t::pd_t::init_kernel_ctx(
        compute::kernel_ctx_t &kernel_ctx) const {
    return init_kernel_ctx_common(kernel_ctx, conf);
}
status_t simple_concat_t::execute_concat(const exec_ctx_t &ctx) const {

    const auto &conf = pd()->conf;
    auto &dst = CTX_OUT_STORAGE(DNNL_ARG_DST);

    compute::kernel_arg_list_t arg_list;
    arg_list.set(0, dst);
    for (int i = 0; i < pd()->n_inputs(); ++i) {
        auto &src = CTX_IN_STORAGE(DNNL_ARG_MULTIPLE_SRC + i);
        arg_list.set(i + 1, src);
    }

    auto nd_range = compute::nd_range_t(conf.gws_d, conf.lws_d);

    status_t status = parallel_for(ctx, nd_range, kernel_, arg_list);
    return status;
}
} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl
