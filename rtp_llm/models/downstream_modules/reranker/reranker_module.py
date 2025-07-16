from typing import Any, Dict, List, Tuple

import numpy as np
from transformers import PreTrainedTokenizerBase

from rtp_llm.async_decoder_engine.embedding.interface import EngineInputs, EngineOutputs
from rtp_llm.config.gpt_init_model_parameters import GptInitModelParameters
from rtp_llm.models.downstream_modules.classifier.bert_classifier import (
    BertClassifierHandler,
)
from rtp_llm.models.downstream_modules.classifier.classifier import ClassifierHandler
from rtp_llm.models.downstream_modules.classifier.roberta_classifier import (
    RobertaClassifierHandler,
)
from rtp_llm.models.downstream_modules.common_input_generator import (
    CommonInputGenerator,
)
from rtp_llm.models.downstream_modules.custom_module import CustomModule, CustomRenderer
from rtp_llm.models.downstream_modules.reranker.api_datatype import (
    RankingItem,
    VoyageRerankerRequest,
    VoyageRerankerResponse,
)


class RerankerModule(CustomModule):
    def __init__(
        self, config: GptInitModelParameters, tokenizer: PreTrainedTokenizerBase
    ):
        super().__init__(config, tokenizer)
        self.renderer = RerankerRenderer(self.config_, self.tokenizer_)
        self.handler = ClassifierHandler(self.config_)


class BertRerankerModule(CustomModule):
    def __init__(
        self, config: GptInitModelParameters, tokenizer: PreTrainedTokenizerBase
    ):
        super().__init__(config, tokenizer)
        self.renderer = RerankerRenderer(self.config_, self.tokenizer_)
        self.handler = BertClassifierHandler(self.config_)


class RobertaRerankerModule(CustomModule):
    def __init__(
        self, config: GptInitModelParameters, tokenizer: PreTrainedTokenizerBase
    ):
        super().__init__(config, tokenizer)
        self.renderer = RerankerRenderer(self.config_, self.tokenizer_)
        self.handler = RobertaClassifierHandler(self.config_)


class RerankerRenderer(CustomRenderer):
    def __init__(
        self, config: GptInitModelParameters, tokenizer: PreTrainedTokenizerBase
    ):
        super().__init__(config, tokenizer)
        self.generator = CommonInputGenerator(tokenizer, config)

    def render_request(self, request: Dict[str, Any]):
        return VoyageRerankerRequest(**request)

    @staticmethod
    def sigmoid(x: float):
        return float(1 / (1 + np.exp(-x)))

    def create_input(self, formated_request: VoyageRerankerRequest):
        input: List[Tuple[str, str]] = [
            (formated_request.query, doc) for doc in formated_request.documents
        ]
        return self.generator.generate(input, truncate=formated_request.truncation)

    async def render_response(
        self,
        formated_request: VoyageRerankerRequest,
        inputs: EngineInputs,
        outputs: EngineOutputs,
    ) -> Dict[str, Any]:
        if outputs.outputs is None:
            raise Exception("outputs should not be None")
        rank_items: List[RankingItem] = []
        for i in range(len(formated_request.documents)):
            rank_items.append(
                RankingItem(
                    index=i,
                    document=(
                        formated_request.documents[i]
                        if formated_request.return_documents
                        else None
                    ),
                    relevance_score=(
                        float(outputs.outputs[i])
                        if not formated_request.normalize
                        else self.sigmoid(float(outputs.outputs[i]))
                    ),
                )
            )
        if formated_request.sorted:
            rank_items.sort(key=lambda x: x.relevance_score, reverse=True)
        if formated_request.top_k is not None:
            rank_items = rank_items[: min(len(rank_items), formated_request.top_k)]
        return VoyageRerankerResponse(
            results=rank_items, total_tokens=len(inputs.token_ids)
        ).model_dump(exclude_none=True)
