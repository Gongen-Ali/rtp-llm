from rtp_llm.server.server_args.util import str2bool


def init_parallel_group_args(parser):
    ##############################################################################################################
    # Parallelism and Distributed Setup Configuration
    ##############################################################################################################
    parallel_group = parser.add_argument_group(
        "Parallelism and Distributed Setup Configuration"
    )
    parallel_group.add_argument(
        "--tp_size",
        env_name="TP_SIZE",
        type=int,
        default=None,
        help="指定用于张量并行度。",
    )
    parallel_group.add_argument(
        "--ep_size",
        env_name="EP_SIZE",
        type=int,
        default=None,
        help="定义用于专家并行（Expert Parallelism）的模型（专家）实例数量。",
    )
    parallel_group.add_argument(
        "--dp_size",
        env_name="DP_SIZE",
        type=int,
        default=None,
        help="设置数据并行（Data Parallelism）的副本数量或组大小。",
    )
    parallel_group.add_argument(
        "--world_size",
        env_name="WORLD_SIZE",
        type=int,
        default=None,
        help="分布式设置中使用的GPU总数。通常情况下，`WORLD_SIZE = TP_SIZE * DP_SIZE`",
    )
    parallel_group.add_argument(
        "--world_rank",
        env_name="WORLD_RANK",
        type=int,
        default=None,
        help="当前进程/GPU在分布式系统中的全局唯一编号（从0到 `WORLD_SIZE - 1`）。",
    )
    parallel_group.add_argument(
        "--local_world_size",
        env_name="LOCAL_WORLD_SIZE",
        type=int,
        default=None,
        help="在多节点分布式设置中，当前节点（Node）上使用的GPU设备数量。",
    )
    parallel_group.add_argument(
        "--ffn_sp_size",
        env_name="FFN_SP_SIZE",
        type=int,
        default=1,
        help="FFN层序列并行大小。",
    )
    parallel_group.add_argument(
        "--enable_ffn_disaggregate",
        env_name="ENABLE_FFN_DISAGGREGATE",
        type=str2bool,
        default=False,
        help="启用AF分离功能。",
    )
