import logging
from typing import Optional

from rtp_llm.config.task_type import TaskType
from rtp_llm.model_factory_register import register_model
from rtp_llm.models.downstream_modules.custom_module import CustomModule
from rtp_llm.models.downstream_modules.reranker.qwen3_reranker import (
    Qwen3RerankerModule,
)
from rtp_llm.models.qwen_v2 import QWenV2, QWenV2Weight


class QWenV3Weight(QWenV2Weight):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.bias = False


class QwenV3(QWenV2):
    @staticmethod
    def get_weight_cls():
        return QWenV3Weight

    @classmethod
    def _create_config(cls, ckpt_path: str):
        config = super()._create_config(ckpt_path)
        config.qk_norm = True
        return config

    def _init_custom_module(self) -> Optional[CustomModule]:
        logging.info(f"task_type : {self.task_type}")
        if self.task_type == TaskType.RERANKER:
            logging.info("using Qwen3RerankerModule as custom module")
            return Qwen3RerankerModule(self.config, self.tokenizer)
        return super()._init_custom_module()


register_model("qwen_3", QwenV3, ["Qwen3ForCausalLM"])
register_model("qwen_3_tool", QwenV3)
