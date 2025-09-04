from rtp_llm.server.server_args.util import str2bool


def init_gang_group_args(parser):
    ##############################################################################################################
    # Gang Configuration
    ##############################################################################################################
    gang_group = parser.add_argument_group("Gang Configuration")
    gang_group.add_argument(
        "--fake_gang_env",
        env_name="FAKE_GANG_ENV",
        type=str2bool,
        default=False,
        help="在多机启动时的fake行为",
    )
    gang_group.add_argument(
        "--gang_annocation_path",
        env_name="GANG_ANNOCATION_PATH",
        type=str,
        default="/etc/podinfo/annotations",
        help="GANG信息的路径",
    )
    gang_group.add_argument(
        "--gang_config_string",
        env_name="GANG_CONFIG_STRING",
        type=str,
        default=None,
        help="GAG信息的字符串表达",
    )
    gang_group.add_argument(
        "--zone_name", env_name="ZONE_NAME", type=str, default="", help="角色名"
    )
    gang_group.add_argument(
        "--distribute_config_file",
        env_name="DISTRIBUTE_CONFIG_FILE",
        type=str,
        default=None,
        help="分布式的配置文件路径",
    )
    gang_group.add_argument(
        "--dist_barrier_timeout",
        env_name="DIST_BARRIER_TIMEOUT",
        type=int,
        default=45,
        help="心跳检测的超时时间",
    )
    gang_group.add_argument(
        "--gang_sleep_time",
        env_name="GANG_SLEEP_TIME",
        type=int,
        default=10,
        help="心跳检测的间隔时间",
    )
    gang_group.add_argument(
        "--gang_timeout_min",
        env_name="GANG_TIMEOUT_MIN",
        type=int,
        default=30,
        help="心跳超时的最小时间",
    )
