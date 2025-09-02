import argparse
import logging
import os

from rtp_llm.server.server_args.util import str2bool


def init_cache_store_group_args(parser):
    ##############################################################################################################
    # Cache Store 配置
    ##############################################################################################################
    cache_store_group = parser.add_argument_group("Cache Store")
    cache_store_group.add_argument(
        "--cache_store_rdma_mode",
        env_name="CACHE_STORE_RDMA_MODE",
        type=str2bool,
        default=False,
        help="控制 cache store 是否使用 RDMA 模式。",
    )

    cache_store_group.add_argument(
        "--wrr_available_ratio",
        env_name="WRR_AVAILABLE_RATIO",
        type=int,
        default=80,
        help="为 WRR (Weighted Round Robin) 负载均衡器设置的可用性阈值百分比 (0-100)，数值越低越容易启用动态权重分配，但可能降低全局负载均衡准确性。",
    )

    cache_store_group.add_argument(
        "--rank_factor",
        env_name="RANK_FACTOR",
        type=int,
        default=0,
        choices=[0, 1],
        help="指定 WRR 负载均衡器使用的排序因子。`0` 表示基于 KV_CACHE 使用情况排序，`1` 表示基于正在处理的请求数 (ONFLIGHT_REQUESTS) 排序。",
    )

    cache_store_group.add_argument(
        "--cache_store_thread_count",
        env_name="CACHE_STORE_THREAD_COUNT",
        type=int,
        default=16,
        help="为 cache store 线程池设置线程数量。",
    )

    cache_store_group.add_argument(
        "--cache_store_rdma_connect_timeout_ms",
        env_name="CACHE_STORE_RDMA_CONNECT_TIMEOUT_MS",
        type=int,
        default=250,
        help="为 cache store RDMA 连接设置超时时间，单位为毫秒。",
    )

    cache_store_group.add_argument(
        "--cache_store_rdma_qp_count_per_connection",
        env_name="CACHE_STORE_RDMA_QP_COUNT_PER_CONNECTION",
        type=int,
        default=2,
        help="为 cache store RDMA 连接设置每个连接的底层QP数量。",
    )
