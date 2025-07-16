from typing import Any, Dict, List, Union

import torch
from pydantic import BaseModel
from transformers import PreTrainedTokenizerBase

from rtp_llm.async_decoder_engine.embedding.interface import EngineOutputs
from rtp_llm.config.base_model_config import PyDanticModelBase
from rtp_llm.config.gpt_init_model_parameters import GptInitModelParameters
from rtp_llm.embedding.embedding_type import TYPE_STR, EmbeddingType
from rtp_llm.model_loader.weight_module import CustomAtomicWeight
from rtp_llm.models.downstream_modules.custom_module import (
    CustomHandler,
    CustomModule,
    CustomRenderer,
)
from rtp_llm.models.downstream_modules.embedding.api_datatype import (
    ColbertEmbeddingRequest,
    OpenAIEmbeddingRequest,
    SparseEmbeddingRequest,
)
from rtp_llm.models.downstream_modules.embedding.colbert_embedding_module import (
    ColBertEmbeddingHandler,
    ColBertEmbeddingModule,
)
from rtp_llm.models.downstream_modules.embedding.dense_embedding_module import (
    DenseEmbeddingModule,
    SentenceTransformerHandler,
)
from rtp_llm.models.downstream_modules.embedding.misc import combo_to_batch
from rtp_llm.models.downstream_modules.embedding.sparse_emebdding_module import (
    SparseEmbeddingHandler,
    SparseEmbeddingModule,
)


class RequestTuple(PyDanticModelBase):
    emb_type: EmbeddingType
    request: BaseModel


class BgeM3EmbeddingModule(CustomModule):
    def __init__(
        self, config: GptInitModelParameters, tokenizer: PreTrainedTokenizerBase
    ):
        self._colbert_module = ColBertEmbeddingModule(config, tokenizer)
        self._sparse_module = SparseEmbeddingModule(config, tokenizer)
        self._dense_module = DenseEmbeddingModule(config, tokenizer)
        # not in use
        self.renderer = CustomRenderer(config, tokenizer)

        self.module_map = {
            EmbeddingType.DENSE: self._dense_module,
            EmbeddingType.SPARSE: self._sparse_module,
            EmbeddingType.COLBERT: self._colbert_module,
        }

        self.handler = BgeM3EmbeddingHandler(
            self._dense_module.handler,
            self._sparse_module.handler,
            self._colbert_module.handler,
        )

    def custom_weight_info(self) -> List[CustomAtomicWeight]:
        return []

    def get_renderer(self, request: Dict[str, Any]) -> CustomRenderer:
        request_type = request.get(TYPE_STR, EmbeddingType.DENSE)
        if request_type not in self.module_map:
            raise Exception(
                "BgeM3EmbeddingModule get unexpected type str: " + str(request_type)
            )
        return self.module_map[request_type].renderer

    def init(self, tensor_map: Dict[str, torch.Tensor]) -> None:
        self.handler.init(tensor_map)


class BgeM3EmbeddingHandler(CustomHandler):
    def __init__(
        self,
        dense: SentenceTransformerHandler,
        sparse: SparseEmbeddingHandler,
        colbert: ColBertEmbeddingHandler,
    ):
        self.dense_handler = dense
        self.sparse_handler = sparse
        self.colbert_handler = colbert

    def init(self, tensor_map: Any):
        self.dense_handler.init(tensor_map)
        self.sparse_handler.init(tensor_map)
        self.colbert_handler.init(tensor_map)

    def _get_embedding_type(
        self,
        request: Union[
            OpenAIEmbeddingRequest, SparseEmbeddingRequest, ColbertEmbeddingRequest
        ],
    ):
        if type(request) is OpenAIEmbeddingRequest:
            return EmbeddingType.DENSE
        if type(request) is SparseEmbeddingRequest:
            return EmbeddingType.SPARSE
        if type(request) is ColbertEmbeddingRequest:
            return EmbeddingType.COLBERT
        raise Exception(
            "unknown request type for BgeM3EmbeddingHandler: " + str(type(request))
        )

    def post_process(
        self,
        request: Union[
            OpenAIEmbeddingRequest, SparseEmbeddingRequest, ColbertEmbeddingRequest
        ],
        batch_output: EngineOutputs,
    ) -> EngineOutputs:
        embedding_type = self._get_embedding_type(request)
        if not isinstance(batch_output.outputs, list):
            raise Exception("BgeM3EmbeddingHandler output should be list")
        batch_output.outputs = [x[embedding_type.value] for x in batch_output.outputs]
        return batch_output

    def forward(
        self,
        input_ids: torch.Tensor,
        hidden_states: torch.Tensor,
        input_lengths: torch.Tensor,
    ) -> List[Dict[str, torch.Tensor]]:
        batch_input_ids, batch_hidden_states, batch_attention_mask = combo_to_batch(
            hidden_states, input_ids, input_lengths
        )

        dense_res = self.dense_handler.forward_internal(
            batch_input_ids, batch_hidden_states, batch_attention_mask
        )
        colbert_res = self.colbert_handler.forward_internal(
            batch_input_ids, batch_hidden_states, batch_attention_mask
        )
        sparse_res = self.sparse_handler.forward(
            input_ids, hidden_states, input_lengths
        )
        stream = torch.cuda.current_stream()
        dense_res = dense_res.cpu()
        colbert_res = colbert_res.cpu()
        sparse_res = sparse_res.cpu()
        # this synchronize requires no gil
        stream.synchronize()
        return [
            {
                EmbeddingType.DENSE.value: d,
                EmbeddingType.SPARSE.value: s,
                EmbeddingType.COLBERT.value: c,
            }
            for d, s, c in zip(dense_res, sparse_res, colbert_res)
        ]
