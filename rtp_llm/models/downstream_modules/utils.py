import os
from typing import Optional
from transformers import PreTrainedTokenizerBase

from rtp_llm.config.task_type import TaskType
from rtp_llm.utils.util import get_config_from_path
from rtp_llm.config.gpt_init_model_parameters import GptInitModelParameters
from rtp_llm.models.downstream_modules.plugin_loader import UserModuleLoader
from rtp_llm.models.downstream_modules import SparseEmbeddingModule, DenseEmbeddingModule, \
    ALLEmbeddingModule, ColBertEmbeddingModule, ClassifierModule, BgeM3EmbeddingModule, RerankerModule

def create_custom_module(task_type: TaskType, config: GptInitModelParameters, tokenizer: Optional[PreTrainedTokenizerBase]):
    # try import internal module
    try:
        from internal_source.rtp_llm.models.downstream_modules.utils import create_custom_module
        internal_module = create_custom_module(task_type, config, tokenizer)
        if internal_module is not None:
            return internal_module
    except ImportError:
        pass

    if task_type == TaskType.LANGUAGE_MODEL:
        return None
    assert tokenizer is not None, "tokenizer should not be None"
    if task_type == TaskType.DENSE_EMBEDDING:
        return DenseEmbeddingModule(config, tokenizer)
    elif task_type == TaskType.ALL_EMBEDDING:
        return ALLEmbeddingModule(config, tokenizer)
    elif task_type == TaskType.SPARSE_EMBEDDING:
        return SparseEmbeddingModule(config, tokenizer)
    elif task_type == TaskType.COLBERT_EMBEDDING:
        return ColBertEmbeddingModule(config, tokenizer)
    elif task_type == TaskType.SEQ_CLASSIFICATION:
        return ClassifierModule(config, tokenizer)
    elif task_type == TaskType.BGE_M3:
        return BgeM3EmbeddingModule(config, tokenizer)
    elif task_type == TaskType.RERANKER:
        return RerankerModule(config, tokenizer)
    raise Exception(f"unknown task_type: {task_type}")