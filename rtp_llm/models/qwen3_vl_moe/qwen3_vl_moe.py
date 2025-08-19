import torch
import torch.nn as nn
import torch.nn.functional as F
from transformers.activations import ACT2FN

from rtp_llm.config.gpt_init_model_parameters import GptInitModelParameters
from rtp_llm.model_factory_register import register_model
from rtp_llm.models.multimodal.multimodal_mixin import (
    BaseMultiModalWeightInfo,
    BaseVitWeights,
    MultiModalMixin,
)
from rtp_llm.models.qwen2_5_vl.modeling_qwen2_5_vl import (
    Qwen2_5_VisionTransformerPretrainedModel,
    Qwen2_5_VLVisionConfig,
)
from rtp_llm.models.qwen2_5_vl.qwen2_5_vl import QWen2_5_VL, Qwen2_5_VLImageEmbedding
from rtp_llm.models.qwen2_vl.qwen2_vl import (
    QWen2_VL,
    QWen2VLWeightInfo,
    QwenVL2VitWeight,
)
from rtp_llm.models.qwen3_vl_moe.modeling_qwen3_vl_moe import (
    Qwen3_VL_MOEVisionTransformerPretrainedModel,
)
from rtp_llm.models.qwen_v3_moe import Qwen3Moe, QWenV3MoeWeight

# === Vision Encoder === #


class Qwen3_VisionMlp(nn.Module):
    def __init__(self, config, bias: bool = False):
        super().__init__()
        self.hidden_size = config.hidden_size
        self.intermediate_size = config.intermediate_size
        self.linear_fc1 = nn.Linear(self.hidden_size, self.intermediate_size, bias=bias)
        self.linear_act = ACT2FN[config.hidden_act]
        self.linear_fc2 = nn.Linear(self.intermediate_size, self.hidden_size, bias=bias)

    def forward(self, x) -> torch.Tensor:
        return self.linear_fc2(self.linear_act(self.linear_fc1(x)))


# class Qwen3_VisionMLP(nn.Module):

#     def __init__(self,
#                  in_features: int,
#                  hidden_features: int,
#                  bias: bool = False,
#                  act_fn: Callable[[torch.Tensor], torch.Tensor] = F.silu,
#                  quant_config: Optional[QuantizationConfig] = None,
#                  prefix: str = ""):
#         super().__init__()
#         self.linear_fc1 = ColumnParallelLinear(in_features,
#                                                hidden_features,
#                                                bias=bias,
#                                                quant_config=quant_config,
#                                                return_bias=False,
#                                                prefix=f"{prefix}.linear_fc1")
#         self.linear_fc2 = RowParallelLinear(hidden_features,
#                                             in_features,
#                                             bias=bias,
#                                             quant_config=quant_config,
#                                             return_bias=False,
#                                             prefix=f"{prefix}.linear_fc2")
#         self.act_fn = act_fn

#     def forward(self, x: torch.Tensor):
#         mlp_output = self.linear_fc2(self.act_fn(self.linear_fc1(x)))
#         return mlp_output


class QWenV3VLWeightInfo(QWenV3MoeWeight, BaseMultiModalWeightInfo):
    def __init__(self, config, tp_size, tp_rank):
        QWenV3MoeWeight.__init__(self, config, tp_size, tp_rank)
        BaseMultiModalWeightInfo.__init__(self, config)
        self.bias = False
        self.use_qk_norm = True

    @property
    def support_lora(self) -> bool:
        return True

    def _get_weight_info(self):
        weights = self._get_hf_weight_info()
        weights = self._get_vit_info(weights)
        return weights


class QWen3_VL_MOE(QWen2_5_VL):
    def _init_multimodal(self, config: GptInitModelParameters):
        self.mm_part = Qwen2_5_VLImageEmbedding(config)
        self.mm_part.visual = Qwen3_VL_MOEVisionTransformerPretrainedModel(
            config.mm_related_params.config
        )
        # vl_config = Qwen2_5_VLVisionConfig(**config.mm_related_params.config)

        # for i in range(len(self.mm_part.visual.blocks)):
        #     self.mm_part.visual.blocks[i].mlp = Qwen3_VisionMlp(vl_config, bias=True)

        config.mm_related_params.vit_weights = QwenVL2VitWeight(
            {"vit": self.mm_part.visual}
        )

    @staticmethod
    def get_weight_cls():
        return QWenV3VLWeightInfo

    @classmethod
    def _create_config(cls, ckpt_path: str):
        config = super()._create_config(ckpt_path)
        Qwen3Moe.load_moe_config(ckpt_path, config)
        config.use_qk_norm = True
        return config


register_model("qwen3_vl_moe", QWen3_VL_MOE, ["Qwen3_VL_MOEForConditionalGeneration"])
