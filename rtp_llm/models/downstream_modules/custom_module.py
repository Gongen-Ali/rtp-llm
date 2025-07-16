import logging
from typing import Any, Dict, List, Union

import torch
from pydantic import BaseModel
from transformers import PreTrainedTokenizerBase

from rtp_llm.async_decoder_engine.embedding.interface import EngineInputs, EngineOutputs
from rtp_llm.config.gpt_init_model_parameters import GptInitModelParameters
from rtp_llm.model_loader.model_weight_info import ModelWeights
from rtp_llm.model_loader.weight_module import CustomAtomicWeight

"""
用于多种多样的下游任务
"""


class CustomModule(object):
    renderer: "CustomRenderer"
    handler: "CustomHandler"

    def __init__(
        self, config: GptInitModelParameters, tokenizer: PreTrainedTokenizerBase
    ):
        self.config_ = config
        self.tokenizer_ = tokenizer

    def create_cpp_handler(self) -> Any:
        raise NotImplementedError("not support cpp handler")

    def get_renderer(self, request: Dict[str, Any]) -> "CustomRenderer":
        return self.renderer

    def get_handler(self) -> "CustomHandler":
        return self.handler

    def get_custom_weight_info(self) -> List[CustomAtomicWeight]:
        return self.handler.custom_weight_info()

    def init(self, weight: ModelWeights):
        tensor_map: Dict[str, torch.Tensor] = {}
        for weight_info in self.get_custom_weight_info():
            if weight_info.name.startswith(CustomAtomicWeight.prefix):
                name = weight_info.name.replace(CustomAtomicWeight.prefix, "", 1)
            else:
                name = weight_info.name
            tensor_map[name] = weight.get_global_weight(weight_info.name)
        self.handler.init(tensor_map)


class CustomHandler(object):
    def __init__(self, config: GptInitModelParameters):
        self.config_ = config
        self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    def custom_weight_info(self) -> List[CustomAtomicWeight]:
        return []

    # for cpp
    def init_cpp_handler(self) -> None:
        pass

    def init(self, tensor_map: Dict[str, torch.Tensor]) -> None:
        pass

    # 输入:
    # input_ids: [token_len]
    # hidden_states: [token_len, hidden_size]
    # input_lengths: [batch_size]
    # 输出:
    # [batch_size], 由endpoint格式化返回
    def forward(
        self,
        input_ids: torch.Tensor,
        hidden_states: torch.Tensor,
        input_lengths: torch.Tensor,
    ) -> Union[torch.Tensor, List[Any]]:
        raise NotImplementedError

    def post_process(self, request: Any, batch_output: EngineOutputs) -> EngineOutputs:
        return batch_output


class CustomRenderer(object):
    def __init__(
        self, config: GptInitModelParameters, tokenizer: PreTrainedTokenizerBase
    ):
        self.config_ = config
        self.tokenizer_ = tokenizer

    def render_request(self, request_json: Dict[str, Any]) -> BaseModel:
        raise NotImplementedError

    def create_input(self, request: BaseModel) -> EngineInputs:
        raise NotImplementedError

    async def render_response(
        self, request: BaseModel, inputs: EngineInputs, outputs: EngineOutputs
    ) -> Dict[str, Any]:
        raise NotImplementedError

    async def render_log_response(self, response: Dict[str, Any]):
        return response
