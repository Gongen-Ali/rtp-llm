from rtp_llm.server.server_args.util import str2bool


def init_moe_group_args(parser):
    ##############################################################################################################
    # MOE 特性
    ##############################################################################################################
    moe_group = parser.add_argument_group("MOE 专家并行")
    moe_group.add_argument(
        "--use_deepep_moe",
        env_name="USE_DEEPEP_MOE",
        type=str2bool,
        default=False,
        help="设置为 `True` 以启用 DeepEP 来处理 MoE 模型的 expert 部分。",
    )

    moe_group.add_argument(
        "--use_deepep_internode",
        env_name="USE_DEEPEP_INTERNODE",
        type=str2bool,
        default=False,
        help="设置为 `True` 以启用 DeepEP 来优化跨节点 (inter-node) 通信。",
    )

    moe_group.add_argument(
        "--use_deepep_low_latency",
        env_name="USE_DEEPEP_LOW_LATENCY",
        type=str2bool,
        default=True,
        help="设置为 `True` 以启用 DeepEP 的低延迟模式。",
    )

    moe_group.add_argument(
        "--use_deepep_p2p_low_latency",
        env_name="USE_DEEPEP_P2P_LOW_LATENCY",
        type=str2bool,
        default=False,
        help="设置为 `True` 以启用 DeepEP 的点对点 (P2P) 低延迟模式。",
    )

    moe_group.add_argument(
        "--deep_ep_num_sm",
        env_name="DEEP_EP_NUM_SM",
        type=int,
        default=0,
        help="为 DeepEPBuffer 设置 SM (Streaming Multiprocessor) 数量。设置为 `0` 将使用系统默认配置。",
    )

    moe_group.add_argument(
        "--fake_balance_expert",
        env_name="FAKE_BALANCE_EXPERT",
        type=str2bool,
        default=False,
        help="设置为 `True` 时，为 MoE 模型中的 expert 启用伪均衡 (fake balancing) 机制。用于测试或模拟特定均衡行为。",
    )

    moe_group.add_argument(
        "--eplb_control_step",
        env_name="EPLB_CONTROL_STEP",
        type=int,
        default=100,
        help="为 EPLB (Expert Placement Load Balancing) 控制器指定控制周期或步骤参数。这可能影响专家的负载均衡调整的频率或粒度。",
    )

    moe_group.add_argument(
        "--eplb_test_mode",
        env_name="EPLB_TEST_MODE",
        type=str2bool,
        default=False,
        help="设置为 `True` 时，为 ExpertBalancer 组件启用测试模式。用于调试或特定的测试场景。",
    )

    moe_group.add_argument(
        "--eplb_balance_layer_per_step",
        env_name="EPLB_BALANCE_LAYER_PER_STEP",
        type=int,
        default=1,
        help="设置 eplb 每次更新的层数。",
    )

    moe_group.add_argument(
        "--eplb_mode",
        env_name="EPLB_MODE",
        type=str,
        default="NONE",
        help="专家并行的负载均衡模式",
    )
    moe_group.add_argument(
        "--eplb_update_time",
        env_name="EPLB_UPDATE_TIME",
        type=int,
        default=5000,
        help="专家并行复杂均衡的更新时间",
    )
    moe_group.add_argument(
        "--redundant_expert",
        env_name="REDUNDANT_EXPERT",
        type=int,
        default=0,
        help="冗余专家个数",
    )
    moe_group.add_argument(
        "--hack_ep_single_entry",
        env_name="HACK_EP_SINGLE_ENTRY",
        type=int,
        default=0,
        help="HACK_EP_SINGLE_ENTRY",
    )
    moe_group.add_argument(
        "--balance_method",
        env_name="BALANCE_METHOD",
        type=str,
        default="mix",
        help="负载均衡的方法",
    )
    moe_group.add_argument(
        "--eplb_force_repack",
        env_name="EPLB_FORCE_REPACK",
        type=int,
        default=0,
        help="EPLB_FORCE_REPACK",
    )
    moe_group.add_argument(
        "--eplb_stats_window_size",
        env_name="EPLB_STATS_WINDOW_SIZE",
        type=int,
        default=10,
        help="负载均衡的统计窗口大小",
    )
    moe_group.add_argument(
        "--max_moe_normal_masked_token_num",
        env_name="RTP_LLM_MAX_MOE_NORMAL_MASKED_TOKEN_NUM",
        type=int,
        default=1024,
        help="moe normal使用masked的最大token数目",
    )
