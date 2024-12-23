#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "maga_transformer/cpp/api_server/GenerateStreamWrapper.h"

#include "maga_transformer/cpp/api_server/test/mock/MockEngineBase.h"
#include "maga_transformer/cpp/api_server/test/mock/MockGenerateStream.h"
#include "maga_transformer/cpp/api_server/test/mock/MockTokenProcessor.h"
#include "maga_transformer/cpp/api_server/test/mock/MockApiServerMetricReporter.h"

using namespace ::testing;

namespace rtp_llm {

std::shared_ptr<MockGenerateStream> CreateMockGenerateStream() {
    auto input             = std::make_shared<GenerateInput>();
    input->generate_config = std::make_shared<GenerateConfig>();

    auto fake_token_ids       = std::vector<int>{1, 2, 3, 4, 5};
    std::vector<size_t> shape = {fake_token_ids.size()};
    // 由于 Buffer 内部不负责管理传入的地址数据(只是使用), 所以数据必须具有较久的生命周期
    input->input_ids =
        std::make_shared<ft::Buffer>(ft::MemoryType::MEMORY_CPU, ft::DataType::TYPE_INT32, shape, fake_token_ids.data());

    ft::GptInitParameter param;
    param.max_seq_len_ = fake_token_ids.size();

    auto mock_stream = std::make_shared<MockGenerateStream>(input, param);
    return mock_stream;
}

class GenerateStreamWrapperTest: public Test {};

TEST_F(GenerateStreamWrapperTest, generateResponse) {

    auto mock_engine_ = std::make_shared<MockEngineBase>();
    auto engine  = std::dynamic_pointer_cast<EngineBase>(mock_engine_);

    auto mock_token_processor_ = std::make_shared<MockTokenProcessor>();
    auto token_processor  = std::dynamic_pointer_cast<TokenProcessor>(mock_token_processor_);

    auto mock_metric_reporter_ = std::make_shared<MockApiServerMetricReporter>();
    auto metric_reporter  = std::dynamic_pointer_cast<ApiServerMetricReporter>(mock_metric_reporter_);

    auto mock_stream = CreateMockGenerateStream();
    auto stream      = std::dynamic_pointer_cast<GenerateStream>(mock_stream);
    EXPECT_CALL(*mock_engine_, enqueue(Matcher<const std::shared_ptr<GenerateInput>&>(_))).WillOnce(Return(stream));

    GenerateOutputs outputs;
    EXPECT_CALL(*mock_stream, nextOutput())
        .WillOnce(Return(ErrorResult<GenerateOutputs>(std::move(outputs))))
        .WillOnce(Return(ErrorResult<GenerateOutputs>(ErrorCode::OUTPUT_QUEUE_IS_EMPTY, "output queue is empty")));

    EXPECT_CALL(*mock_stream, finished())
        .Times(2)
        .WillRepeatedly(Return(false));

    EXPECT_CALL(*mock_token_processor_, getTokenProcessorCtx(_,_,_)).WillOnce(Return(nullptr));
    EXPECT_CALL(*mock_token_processor_, decodeTokens(_,_,_,_)).WillOnce(Return(std::vector<std::string>()));
    EXPECT_CALL(*mock_metric_reporter_, reportFTPostTokenProcessorRtMetric).WillOnce(Invoke([](double val) {
        EXPECT_TRUE(val >= 0);
    }));

    auto input = std::make_shared<GenerateInput>();
    input->generate_config = std::make_shared<GenerateConfig>();
    auto fake_token_ids = std::vector<int>{1, 2, 3, 4, 5};
    std::vector<size_t> shape = {fake_token_ids.size()};
    // 由于 Buffer 内部不负责管理传入的地址数据(只是使用), 所以数据必须具有较久的生命周期
    input->input_ids =
        std::make_shared<ft::Buffer>(ft::MemoryType::MEMORY_CPU, ft::DataType::TYPE_INT32, shape, fake_token_ids.data());

    GenerateStreamWrapper stream_wrapper(metric_reporter, token_processor);
    stream_wrapper.init(input, engine);
    EXPECT_TRUE(stream_wrapper.generate_config_ != nullptr);
    EXPECT_EQ(stream_wrapper.input_ids_->type(), ft::DataType::TYPE_INT32);
    EXPECT_EQ(stream_wrapper.input_ids_->size(), fake_token_ids.size());
    EXPECT_EQ(stream_wrapper.input_ids_->sizeBytes(), fake_token_ids.size() * sizeof(int));
    EXPECT_TRUE(std::memcmp(stream_wrapper.input_ids_->data(),
                            fake_token_ids.data(),
                            stream_wrapper.input_ids_->sizeBytes()) == 0);
    {
        auto [response, finished] = stream_wrapper.generateResponse();
        ASSERT_EQ(finished, false);
    }
    {
        auto [response, finished] = stream_wrapper.generateResponse();
        ASSERT_EQ(finished, true);
    }
    // EXPECT_CALL(*mock_stream, finished()).WillOnce(Return(false));
    // EXPECT_CALL(*mock_stream, stopped()).WillOnce(Return(false));
    // EXPECT_CALL(*mock_stream, cancel()).Times(1);
}

TEST_F(GenerateStreamWrapperTest, formatResponse_NumBeams) {
    std::vector<std::string> generate_texts;
    generate_texts.push_back("fake response");
    GenerateOutputs generate_outputs;
    generate_outputs.generate_outputs.push_back(GenerateOutput());
    auto generate_config = std::make_shared<GenerateConfig>();
    ft::BufferPtr input_ids;

    generate_config->num_beams = 2;
    auto res = GenerateStreamWrapper::formatResponse(generate_texts,
                                                     generate_outputs,
                                                     generate_config,
                                                     input_ids);
    ASSERT_EQ(res.response, generate_texts);
    ASSERT_EQ(res.aux_info[0].beam_responses, generate_texts);
}

TEST_F(GenerateStreamWrapperTest, formatResponse_Logits) {
    ft::BufferPtr input_ids;

    std::vector<std::string> generate_texts;
    generate_texts.push_back("fake response");

    GenerateOutput generate_output;
    auto fake_token_ids       = std::vector<float>{1, 2, 3, 4, 5};
    std::vector<size_t> shape = {fake_token_ids.size()};
    generate_output.logits = std::make_shared<ft::Buffer>(ft::MemoryType::MEMORY_CPU,
                                                          ft::DataType::TYPE_FP32,
                                                          shape,
                                                          fake_token_ids.data());
    GenerateOutputs generate_outputs;
    generate_outputs.generate_outputs.push_back(generate_output);

    auto generate_config = std::make_shared<GenerateConfig>();
    generate_config->return_logits = true;

    auto res = GenerateStreamWrapper::formatResponse(generate_texts,
                                                     generate_outputs,
                                                     generate_config,
                                                     input_ids);
    ASSERT_EQ(res.response, generate_texts);
    ASSERT_EQ(res.logits.has_value(), true);

    auto logits = res.logits.value();
    EXPECT_NEAR(logits[0][0], 1, 0.001);
    EXPECT_NEAR(logits[0][1], 2, 0.001);
    EXPECT_NEAR(logits[0][2], 3, 0.001);
    EXPECT_NEAR(logits[0][3], 4, 0.001);
    EXPECT_NEAR(logits[0][4], 5, 0.001);
}

TEST_F(GenerateStreamWrapperTest, formatResponse_Loss) {
    ft::BufferPtr input_ids;

    std::vector<std::string> generate_texts;
    generate_texts.push_back("fake response");

    GenerateOutput generate_output;
    auto fake_token_ids       = std::vector<float>{1, 2, 3, 4, 5};
    std::vector<size_t> shape = {fake_token_ids.size()};
    generate_output.loss = std::make_shared<ft::Buffer>(ft::MemoryType::MEMORY_CPU,
                                                        ft::DataType::TYPE_FP32,
                                                        shape,
                                                        fake_token_ids.data());
    GenerateOutputs generate_outputs;
    generate_outputs.generate_outputs.push_back(generate_output);

    auto generate_config = std::make_shared<GenerateConfig>();
    generate_config->calculate_loss = true;

    auto res = GenerateStreamWrapper::formatResponse(generate_texts,
                                                     generate_outputs,
                                                     generate_config,
                                                     input_ids);
    ASSERT_EQ(res.response, generate_texts);
    ASSERT_EQ(res.loss.has_value(), true);

    auto loss = res.loss.value();
    EXPECT_NEAR(loss[0][0], 1, 0.001);
    EXPECT_NEAR(loss[0][1], 2, 0.001);
    EXPECT_NEAR(loss[0][2], 3, 0.001);
    EXPECT_NEAR(loss[0][3], 4, 0.001);
    EXPECT_NEAR(loss[0][4], 5, 0.001);
}

TEST_F(GenerateStreamWrapperTest, formatResponse_HiddenStates) {
    ft::BufferPtr input_ids;

    std::vector<std::string> generate_texts;
    generate_texts.push_back("fake response");

    GenerateOutput generate_output;
    auto fake_token_ids       = std::vector<float>{1, 2, 3, 4, 5};
    std::vector<size_t> shape = {fake_token_ids.size()};
    generate_output.hidden_states = std::make_shared<ft::Buffer>(ft::MemoryType::MEMORY_CPU,
                                                                 ft::DataType::TYPE_FP32,
                                                                 shape,
                                                                 fake_token_ids.data());
    GenerateOutputs generate_outputs;
    generate_outputs.generate_outputs.push_back(generate_output);

    auto generate_config = std::make_shared<GenerateConfig>();
    generate_config->return_hidden_states = true;

    auto res = GenerateStreamWrapper::formatResponse(generate_texts,
                                                     generate_outputs,
                                                     generate_config,
                                                     input_ids);
    ASSERT_EQ(res.response, generate_texts);
    ASSERT_EQ(res.hidden_states.has_value(), true);

    auto hidden_states = res.hidden_states.value();
    EXPECT_NEAR(hidden_states[0][0], 1, 0.001);
    EXPECT_NEAR(hidden_states[0][1], 2, 0.001);
    EXPECT_NEAR(hidden_states[0][2], 3, 0.001);
    EXPECT_NEAR(hidden_states[0][3], 4, 0.001);
    EXPECT_NEAR(hidden_states[0][4], 5, 0.001);
}

TEST_F(GenerateStreamWrapperTest, formatResponse_OutputIds) {
    ft::BufferPtr input_ids;

    std::vector<std::string> generate_texts;
    generate_texts.push_back("fake response");

    GenerateOutput generate_output;
    auto fake_token_ids       = std::vector<int>{1, 2, 3, 4, 5};
    std::vector<size_t> shape = {fake_token_ids.size()};
    generate_output.output_ids = std::make_shared<ft::Buffer>(ft::MemoryType::MEMORY_CPU,
                                                              ft::DataType::TYPE_INT32,
                                                              shape,
                                                              fake_token_ids.data());
    GenerateOutputs generate_outputs;
    generate_outputs.generate_outputs.push_back(generate_output);

    auto generate_config = std::make_shared<GenerateConfig>();
    generate_config->return_output_ids = true;

    auto res = GenerateStreamWrapper::formatResponse(generate_texts,
                                                     generate_outputs,
                                                     generate_config,
                                                     input_ids);
    ASSERT_EQ(res.response, generate_texts);
    ASSERT_EQ(res.output_ids.has_value(), true);

    auto output_ids = res.output_ids.value();
    EXPECT_EQ(output_ids[0][0], 1);
    EXPECT_EQ(output_ids[0][1], 2);
    EXPECT_EQ(output_ids[0][2], 3);
    EXPECT_EQ(output_ids[0][3], 4);
    EXPECT_EQ(output_ids[0][4], 5);
}

TEST_F(GenerateStreamWrapperTest, formatResponse_InputIds) {
    std::vector<std::string> generate_texts;
    generate_texts.push_back("fake response");

    auto fake_token_ids       = std::vector<int>{1, 2, 3, 4, 5};
    std::vector<size_t> shape = {fake_token_ids.size()};
    ft::BufferPtr input_ids = std::make_shared<ft::Buffer>(ft::MemoryType::MEMORY_CPU,
                                                           ft::DataType::TYPE_INT32,
                                                           shape,
                                                           fake_token_ids.data());
    GenerateOutput generate_output;
    GenerateOutputs generate_outputs;
    generate_outputs.generate_outputs.push_back(generate_output);

    auto generate_config = std::make_shared<GenerateConfig>();
    generate_config->return_input_ids = true;

    auto res = GenerateStreamWrapper::formatResponse(generate_texts,
                                                     generate_outputs,
                                                     generate_config,
                                                     input_ids);
    ASSERT_EQ(res.response, generate_texts);
    ASSERT_EQ(res.input_ids.has_value(), true);

    auto input_ids_vec = res.input_ids.value();
    EXPECT_EQ(input_ids_vec[0][0], 1);
    EXPECT_EQ(input_ids_vec[0][1], 2);
    EXPECT_EQ(input_ids_vec[0][2], 3);
    EXPECT_EQ(input_ids_vec[0][3], 4);
    EXPECT_EQ(input_ids_vec[0][4], 5);
}

}  // namespace rtp_llm
