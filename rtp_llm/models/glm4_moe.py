import functools
import json
import logging
import os
from typing import Any, Dict, List

import torch

from rtp_llm.config.gpt_init_model_parameters import GptInitModelParameters
from rtp_llm.model_factory_register import register_model
from rtp_llm.model_loader.attn_weight import AttnAtomicWeight, AttnConfig
from rtp_llm.model_loader.ffn_weight import (
    FfnAtomicWeight,
    FfnConfig,
    FfnWeight,
    MoeAtomicWeight,
    MoeConfig,
    MoeWithSharedWeight,
)
from rtp_llm.model_loader.model_weight_info import (
    ModelDeployWeightInfo,
    ModelWeightInfo,
)
from rtp_llm.model_loader.weight_module import AtomicWeight, WeightModule
from rtp_llm.models.deepseek_v2 import DeepSeekV2
from rtp_llm.utils.model_weight import (
    CkptWeightInfo,
    W,
    identity,
    merge_qkv_b,
    merge_qkv_hf,
    merge_qkv_lora_A,
    merge_qkv_lora_B,
    sp_0,
    sp_head_lora,
    sp_id,
    stack_,
    stack_moe_w1,
    transpose,
    transpose_pad,
)


class Glm4MoeWeight(ModelDeployWeightInfo):
    def __init__(self, *args: Any, **kwargs: Any):
        super().__init__(*args, **kwargs)
        self.prefix = ""
        self.bias = True
        self.has_e_score_correction_bias = False

    def _process_meta(self, meta_dict, weight_keys):
        for layer_id in range(self._num_layers):
            if (
                f"model.layers.{layer_id}.mlp.gate.e_score_correction_bias"
                in weight_keys
            ):
                logging.info(f"found e_score_correction_bias in layer {layer_id}")
                self.has_e_score_correction_bias = True
                break
        return

    def _get_hf_layer_weight_info(self, layer_id: int):
        attn_config = AttnConfig(
            hidden_size=self._hidden_size,
            size_per_head=self._size_per_head,
            head_num=self._head_num,
            head_num_kv=self._head_num_kv,
        )

        layer_weights = [
            AtomicWeight(
                W.pre_ln_gamma,
                [
                    CkptWeightInfo(
                        self.prefix + "model.layers.{i}.input_layernorm.weight",
                        identity,
                    )
                ],
                identity,
            ),
            AttnAtomicWeight(
                W.attn_qkv_w,
                [
                    CkptWeightInfo(
                        self.prefix + "model.layers.{i}.self_attn.q_proj.weight",
                        identity,
                    ),
                    CkptWeightInfo(
                        self.prefix + "model.layers.{i}.self_attn.k_proj.weight",
                        identity,
                    ),
                    CkptWeightInfo(
                        self.prefix + "model.layers.{i}.self_attn.v_proj.weight",
                        identity,
                    ),
                ],
                functools.partial(merge_qkv_hf),
                config=attn_config,
                lora_a_process_func=functools.partial(
                    merge_qkv_lora_A,
                    allow_empty=False,
                    hidden_size=self._hidden_size,
                    head_num=self._head_num,
                    head_num_kv=self._head_num_kv,
                    size_per_head=self._size_per_head,
                ),
                lora_b_process_func=functools.partial(
                    merge_qkv_lora_B,
                    allow_empty=False,
                    hidden_size=self._hidden_size,
                    head_num=self._head_num,
                    head_num_kv=self._head_num_kv,
                    size_per_head=self._size_per_head,
                ),
                lora_a_split_func=sp_id,
                lora_b_split_func=sp_head_lora,
            ),
            AttnAtomicWeight(
                W.attn_o_w,
                [
                    CkptWeightInfo(
                        self.prefix + "model.layers.{i}.self_attn.o_proj.weight",
                        identity,
                    )
                ],
                transpose,
                config=attn_config,
                lora_a_process_func=transpose,
                lora_b_process_func=transpose,
                lora_a_split_func=sp_0,
                lora_b_split_func=sp_id,
            ),
            AtomicWeight(
                W.post_ln_gamma,
                [
                    CkptWeightInfo(
                        self.prefix
                        + "model.layers.{i}.post_attention_layernorm.weight",
                        identity,
                    )
                ],
                identity,
                config=attn_config,
            ),
        ]

        if self.bias:
            layer_weights.append(
                AttnAtomicWeight(
                    W.attn_qkv_b,
                    [
                        CkptWeightInfo(
                            self.prefix + "model.layers.{i}.self_attn.q_proj.bias",
                            identity,
                        ),
                        CkptWeightInfo(
                            self.prefix + "model.layers.{i}.self_attn.k_proj.bias",
                            identity,
                        ),
                        CkptWeightInfo(
                            self.prefix + "model.layers.{i}.self_attn.v_proj.bias",
                            identity,
                        ),
                    ],
                    functools.partial(merge_qkv_b),
                    config=attn_config,
                )
            )

        if self._use_qk_norm:
            layer_weights.extend(
                [
                    AttnAtomicWeight(
                        W.q_ln_gamma,
                        [
                            CkptWeightInfo(
                                self.prefix + "model.layers.{i}.self_attn.q_norm.weight"
                            )
                        ],
                        config=attn_config,
                    ),
                    AttnAtomicWeight(
                        W.k_ln_gamma,
                        [
                            CkptWeightInfo(
                                self.prefix + "model.layers.{i}.self_attn.k_norm.weight"
                            )
                        ],
                        config=attn_config,
                    ),
                ]
            )

        layer_weights.extend(self._get_hf_ffn_layer_weight_info(layer_id))
        return layer_weights

    def _get_hf_ffn_layer_weight_info(self, layer_id: int):
        inter_padding_size = (
            self._layer_inter_padding_size[layer_id]
            if self._layer_inter_padding_size
            else self._inter_padding_size
        )

        ffn_config = FfnConfig(
            inter_padding_size=inter_padding_size,
            is_gated_activation=self._is_gated_activation,
            is_moe=False,
        )

        if layer_id in self.moe_layer_index_:
            moe_config = MoeConfig(
                inter_padding_size=inter_padding_size,
                expert_num=self.expert_num_,
                routed_scaling_factor=self.routed_scaling_factor,
            )
            layer_weights = [
                MoeWithSharedWeight(
                    sub_weights=[
                        MoeAtomicWeight(
                            W.moe_gate,
                            [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.gate.weight", identity
                                )
                            ],
                            transpose,
                            config=moe_config,
                        ),
                        FfnAtomicWeight(
                            W.ffn_w1,
                            [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.shared_experts.gate_proj.weight",
                                    identity,
                                )
                            ],
                            functools.partial(
                                transpose_pad,
                                inter_padding_size=inter_padding_size,
                                dim=0,
                            ),
                            config=ffn_config,
                        ),
                        FfnAtomicWeight(
                            W.ffn_w2,
                            [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.shared_experts.down_proj.weight",
                                    identity,
                                )
                            ],
                            functools.partial(
                                transpose_pad,
                                inter_padding_size=inter_padding_size,
                                dim=1,
                            ),
                            config=ffn_config,
                        ),
                        FfnAtomicWeight(
                            W.ffn_w3,
                            [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.shared_experts.up_proj.weight",
                                    identity,
                                )
                            ],
                            functools.partial(
                                transpose_pad,
                                inter_padding_size=inter_padding_size,
                                dim=0,
                            ),
                            config=ffn_config,
                        ),
                        MoeAtomicWeight(
                            W.moe_w2,
                            [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.experts.{expert_id}.down_proj.weight",
                                    identity,
                                )
                            ],
                            stack_,
                            config=moe_config,
                        ),
                        MoeAtomicWeight(
                            W.moe_w1,
                            [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.experts.{expert_id}.up_proj.weight",
                                    identity,
                                )
                            ]
                            + [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.experts.{expert_id}.gate_proj.weight",
                                    identity,
                                )
                            ],
                            stack_moe_w1,
                            config=moe_config,
                        ),
                    ],
                    config=moe_config,
                )
            ]
            if self.has_e_score_correction_bias:
                layer_weights.append(
                    AtomicWeight(
                        W.e_score_correction_b,
                        [
                            CkptWeightInfo(
                                "model.layers.{i}.mlp.gate.e_score_correction_bias",
                                identity,
                            )
                        ],
                        identity,
                        data_type=torch.float32,
                    )
                )
            return layer_weights
        else:

            return [
                FfnWeight(
                    sub_weights=[
                        FfnAtomicWeight(
                            W.ffn_w1,
                            [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.gate_proj.weight", identity
                                )
                            ],
                            functools.partial(
                                transpose_pad,
                                inter_padding_size=inter_padding_size,
                                dim=0,
                            ),
                            config=ffn_config,
                        ),
                        FfnAtomicWeight(
                            W.ffn_w2,
                            [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.down_proj.weight", identity
                                )
                            ],
                            functools.partial(
                                transpose_pad,
                                inter_padding_size=inter_padding_size,
                                dim=1,
                            ),
                            config=ffn_config,
                        ),
                        FfnAtomicWeight(
                            W.ffn_w3,
                            [
                                CkptWeightInfo(
                                    "model.layers.{i}.mlp.up_proj.weight", identity
                                )
                            ],
                            functools.partial(
                                transpose_pad,
                                inter_padding_size=inter_padding_size,
                                dim=0,
                            ),
                            config=ffn_config,
                        ),
                    ],
                    config=ffn_config,
                ),
            ]

    def _get_weight_info(self):
        layer_weights: List[List[WeightModule]] = []
        weights = [
            AtomicWeight(
                W.embedding,
                [CkptWeightInfo("model.embed_tokens.weight", identity)],
                identity,
            ),
            AtomicWeight(
                W.final_ln_gamma,
                [CkptWeightInfo("model.norm.weight", identity)],
                identity,
            ),
            # AtomicWeight(
            #     W.final_ln_beta, [], functools.partial(zeros, shape=[self._hidden_size])
            # ),
            AtomicWeight(
                W.lm_head, [CkptWeightInfo("lm_head.weight", identity)], identity
            ),
        ]
        for layer in range(self._num_layers):
            layer_weights.append(self._get_hf_layer_weight_info(layer))
        return ModelWeightInfo(layer_weights=layer_weights, weights=weights)


class Glm4Moe(DeepSeekV2):
    @classmethod
    def _create_config(cls, ckpt_path: str):
        config = GptInitModelParameters(
            head_num=0,
            head_num_kv=0,
            size_per_head=0,
            layer_num=0,
            inter_size=0,  # 13696
            vocab_size=152064,
            max_seq_len=8192,
        )
        config.rotary_embedding_style = 1
        config.activation_type = "SiGLU"
        config.has_pre_decoder_layernorm = False
        config.has_post_decoder_layernorm = True
        config.norm_type = "rmsnorm"

        cls._from_hf(config, ckpt_path)
        assert (
            config.head_num > 0
            and config.head_num_kv > 0
            and config.size_per_head > 0
            and config.layer_num > 0
            and config.inter_size > 0
        ), f"error config config.head_num={config.head_num} config.head_num_kv={config.head_num_kv} config.size_per_head={config.size_per_head} config.layer_num={config.layer_num} config.inter_size={config.inter_size}"
        return config

    @classmethod
    def _from_hf(cls, config: GptInitModelParameters, ckpt_path: str):
        config_path = os.path.join(ckpt_path, "config.json")

        if not os.path.exists(config_path):
            return
        with open(config_path) as reader:
            content = reader.read()
            config_json = json.loads(content)
        Glm4Moe._from_config_json(config, config_json)
        logging.info(
            f"glm4 moe config use_qk_norm: {config.qk_norm}, routed_scaling_factor: {config.routed_scaling_factor}"
        )
        return config

    @staticmethod
    def _from_config_json(config: GptInitModelParameters, config_json: Dict[str, Any]):
        config.use_mla = False
        config.inter_size = config_json["intermediate_size"]
        config.head_num = config_json["num_attention_heads"]
        config.head_num_kv = config_json.get("num_key_value_heads", config.head_num)
        config.size_per_head = (
            int(config_json.get("head_dim", 0))
            if "head_dim" in config_json
            else config_json["hidden_size"] // config.head_num
        )
        if config_json.get("hidden_size") is not None:
            config.hidden_size = config_json["hidden_size"]
        config.layer_num = config_json["num_hidden_layers"]
        config.rotary_embedding_base = config_json.get(
            "rope_theta", config.rotary_embedding_base
        )
        config.vocab_size = config_json["vocab_size"]
        config.partial_rotary_factor = config_json.get("partial_rotary_factor", 1.0)
        config.rotary_embedding_dim = int(
            config.size_per_head * config.partial_rotary_factor
        )
        config.layernorm_eps = config_json.get("rms_norm_eps", 1e-06)
        config.tie_word_embeddings = config_json.get("tie_word_embeddings", False)

        config.moe_k = config_json["num_experts_per_tok"]
        config.expert_num = config_json["n_routed_experts"]
        config.moe_inter_padding_size = config_json["moe_intermediate_size"]
        n_shared_experts = config_json["n_shared_experts"]
        config.inter_size = n_shared_experts * config.moe_inter_padding_size
        config.has_moe_norm = config_json.get("norm_topk_prob", False)
        config.moe_style = 2  # shared + expert
        # config.use_qk_norm = config_json.get("use_qk_norm", False)
        config.qk_norm = config_json.get("use_qk_norm", False)
        config.routed_scaling_factor = float(config_json["routed_scaling_factor"])
        config.moe_n_group = config_json.get("n_group", 1)
        config.moe_topk_group = config_json.get("topk_group", 1)
        config.scoring_func = 1  # sigmoid

        first_k_dense_replace = config_json["first_k_dense_replace"]
        config.moe_layer_index = [
            i for i in range(config.layer_num) if i >= first_k_dense_replace
        ]

        ffn_inter_size = config_json.get("intermediate_size", config.inter_size)
        layer_inter_size = []
        for i in range(config.layer_num):
            if i in config.moe_layer_index:
                layer_inter_size.append(config.inter_size)
            else:
                layer_inter_size.append(ffn_inter_size)
        config.layer_inter_size = layer_inter_size

    @staticmethod
    def get_weight_cls() -> type[Glm4MoeWeight]:
        return Glm4MoeWeight


register_model("glm4_moe", Glm4Moe, [], ["Glm4MoeForCausalLM"])
