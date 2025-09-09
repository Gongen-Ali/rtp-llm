from typing import Any, Dict, List

from transformers import AutoTokenizer

from rtp_llm.frontend.tokenizer_factory.tokenizer_factory_register import (
    register_tokenizer,
)
from rtp_llm.frontend.tokenizer_factory.tokenizers.base_tokenizer import BaseTokenizer
from rtp_llm.frontend.tokenizer_factory.tokenizers.tokenization_qwen import (
    QWenTokenizer as QwenTokenizerOrigin,
)


class QWenTokenizer(BaseTokenizer):
    def __init__(self, tokenizer_path: str, config_json: Dict[str, Any] = {}):
        self.tokenizer = QwenTokenizerOrigin.from_pretrained(tokenizer_path)
        self.tokenizer.decoder.update(
            {v: k for k, v in self.tokenizer.special_tokens.items()}
        )

    @property
    def im_start_id(self):
        return self.tokenizer.im_start_id

    @property
    def im_end_id(self):
        return self.tokenizer.im_end_id


class QWenV2Tokenizer(BaseTokenizer):
    def __init__(self, tokenizer_path: str, config_json: Dict[str, Any] = {}):
        self.tokenizer = AutoTokenizer.from_pretrained(
            tokenizer_path, verbose=False, trust_remote_code=True
        )
        self.im_start_id = self.tokenizer.encode("<|im_start|>")[0]
        self.im_end_id = self.tokenizer.encode("<|im_end|>")[0]


register_tokenizer(["qwen", "qwen_7b", "qwen_13b", "qwen_1b8"], QWenTokenizer)
register_tokenizer(
    [
        "qwen_2",
        "qwen_agent",
        "qwen_2_embedding",
        "qwen_tool",
        "qwen_2-mtp",
        "qwen_3",
        "qwen_3_tool",
    ],
    QWenV2Tokenizer,
)
