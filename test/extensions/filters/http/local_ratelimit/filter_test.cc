#include "envoy/extensions/filters/http/local_ratelimit/v3/local_rate_limit.pb.h"

#include "source/common/singleton/manager_impl.h"
#include "source/extensions/filters/http/local_ratelimit/local_ratelimit.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/thread_factory_for_test.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Return;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace LocalRateLimitFilter {

static constexpr absl::string_view config_yaml = R"(
stat_prefix: test
rate_limited_as_resource_exhausted: {}
token_bucket:
  max_tokens: {}
  tokens_per_fill: 1
  fill_interval: 1000s
filter_enabled:
  runtime_key: test_enabled
  default_value:
    numerator: 100
    denominator: HUNDRED
filter_enforced:
  runtime_key: test_enforced
  default_value:
    numerator: 100
    denominator: HUNDRED
response_headers_to_add:
  - append_action: OVERWRITE_IF_EXISTS_OR_ADD
    header:
      key: x-test-rate-limit
      value: 'true'
  - header:
      key: test-resp-req-id
      value: '%REQ(test-req-id)%'
request_headers_to_add_when_not_enforced:
  - append_action: OVERWRITE_IF_EXISTS_OR_ADD
    header:
      key: x-local-ratelimited
      value: 'true'
local_rate_limit_per_downstream_connection: {}
enable_x_ratelimit_headers: {}
  )";
// '{}' used in the yaml config above are position dependent placeholders used for substitutions.
// Different test cases toggle functionality based on these positional placeholder variables
// For instance, fmt::format(config_yaml, "1", "false") substitutes '1' and 'false' for 'max_tokens'
// and 'local_rate_limit_per_downstream_connection' configurations, respectively.

static constexpr absl::string_view simple_config_yaml_without_enforce = R"(
  stat_prefix: test
  token_bucket:
    max_tokens: 1
    tokens_per_fill: 1
    fill_interval: 1000s
  filter_enabled:
    runtime_key: test_enabled
    default_value:
      numerator: 100
      denominator: HUNDRED
)";

class FilterTest : public testing::Test {
public:
  FilterTest() = default;

  void setupPerRoute(const std::string& yaml, const bool enabled = true, const bool enforced = true,
                     const bool per_route = false, const bool has_enabled = true,
                     const bool has_enforced = true) {
    if (has_enabled) {
      EXPECT_CALL(
          factory_context_.runtime_loader_.snapshot_,
          featureEnabled(absl::string_view("test_enabled"),
                         testing::Matcher<const envoy::type::v3::FractionalPercent&>(Percent(100))))
          .WillRepeatedly(testing::Return(enabled));
    } else {
      ASSERT(!enabled); // No filter_enabled and this should be false always.
    }

    if (has_enforced) {
      EXPECT_CALL(
          factory_context_.runtime_loader_.snapshot_,
          featureEnabled(absl::string_view("test_enforced"),
                         testing::Matcher<const envoy::type::v3::FractionalPercent&>(Percent(100))))
          .WillRepeatedly(testing::Return(enforced));
    } else {
      ASSERT(!enforced); // No filter_enforced and this should be false always.
    }

    ON_CALL(decoder_callbacks_, dispatcher()).WillByDefault(ReturnRef(dispatcher_));
    ON_CALL(decoder_callbacks_2_, dispatcher()).WillByDefault(ReturnRef(dispatcher_));

    envoy::extensions::filters::http::local_ratelimit::v3::LocalRateLimit config;
    TestUtility::loadFromYaml(yaml, config);
    config_ =
        std::make_shared<FilterConfig>(config, factory_context_, *stats_.rootScope(), per_route);
    filter_ = std::make_shared<Filter>(config_);
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);

    filter_2_ = std::make_shared<Filter>(config_);
    filter_2_->setDecoderFilterCallbacks(decoder_callbacks_2_);
  }
  void setup(const std::string& yaml, const bool enabled = true, const bool enforced = true) {
    setupPerRoute(yaml, enabled, enforced);
  }

  uint64_t findCounter(const std::string& name) {
    const auto counter = TestUtility::findCounter(stats_, name);
    return counter != nullptr ? counter->value() : 0;
  }

  Http::Code toErrorCode(const uint64_t code) { return config_->toErrorCode(code); }

  Stats::IsolatedStoreImpl stats_;
  testing::NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  testing::NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_2_;
  NiceMock<Event::MockDispatcher> dispatcher_;

  NiceMock<Server::Configuration::MockServerFactoryContext> factory_context_;

  std::shared_ptr<FilterConfig> config_;
  std::shared_ptr<Filter> filter_;
  std::shared_ptr<Filter> filter_2_;
};

TEST_F(FilterTest, Runtime) {
  setup(fmt::format(config_yaml, "false", "1", "false", "\"OFF\""), false, false);
  EXPECT_EQ(&factory_context_.runtime_loader_, &(config_->runtime()));
}

TEST_F(FilterTest, ToErrorCode) {
  setup(fmt::format(config_yaml, "false", "1", "false", "\"OFF\""), false, false);
  EXPECT_EQ(Http::Code::BadRequest, toErrorCode(400));
}

TEST_F(FilterTest, Disabled) {
  setup(fmt::format(config_yaml, "false", "1", "false", "\"OFF\""), false, false);
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
}

TEST_F(FilterTest, NoEnforced) {
  setupPerRoute(std::string(simple_config_yaml_without_enforce), true, false, false, true, false);
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
}

TEST_F(FilterTest, RequestOk) {
  setup(fmt::format(config_yaml, "false", "1", "false", "\"OFF\""));
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration, filter_2_->decodeHeaders(headers, false));
  EXPECT_EQ(2U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.ok"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(FilterTest, RequestOkPerConnection) {
  setup(fmt::format(config_yaml, "false", "1", "true", "\"OFF\""));
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_2_->decodeHeaders(headers, false));
  EXPECT_EQ(2U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(2U, findCounter("test.http_local_rate_limit.ok"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(FilterTest, RequestRateLimited) {
  setup(fmt::format(config_yaml, "false", "1", "false", "\"OFF\""));

  EXPECT_CALL(decoder_callbacks_2_, sendLocalReply(Http::Code::TooManyRequests, _, _, _, _))
      .WillOnce(Invoke([](Http::Code code, absl::string_view body,
                          std::function<void(Http::ResponseHeaderMap & headers)> modify_headers,
                          const absl::optional<Grpc::Status::GrpcStatus> grpc_status,
                          absl::string_view details) {
        EXPECT_EQ(Http::Code::TooManyRequests, code);
        EXPECT_EQ("local_rate_limited", body);

        Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
        modify_headers(response_headers);
        EXPECT_EQ("true", response_headers.get(Http::LowerCaseString("x-test-rate-limit"))[0]
                              ->value()
                              .getStringView());
        // Make sure that generated local reply headers contain a value dynamically
        // generated by header formatter REQ(test-req-id)
        EXPECT_EQ("123", response_headers.get(Http::LowerCaseString("test-resp-req-id"))[0]
                             ->value()
                             .getStringView());
        EXPECT_EQ(grpc_status, absl::nullopt);
        EXPECT_EQ(details, "local_rate_limited");
      }));

  // Add a custom header to the request.
  // Locally generated reply is configured to refer to this value.
  Http::TestRequestHeaderMapImpl request_headers{{"test-req-id", "123"}};
  NiceMock<StreamInfo::MockStreamInfo> stream_info;

  EXPECT_CALL(decoder_callbacks_2_, streamInfo).WillRepeatedly(testing::ReturnRef(stream_info));
  EXPECT_CALL(stream_info, getRequestHeaders).WillRepeatedly(Return(&request_headers));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_2_->decodeHeaders(request_headers, false));
  EXPECT_EQ(2U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.ok"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(FilterTest, RequestRateLimitedResourceExhausted) {
  setup(fmt::format(config_yaml, "true", "1", "false", "\"OFF\""));

  EXPECT_CALL(decoder_callbacks_2_, sendLocalReply(Http::Code::TooManyRequests, _, _, _, _))
      .WillOnce(Invoke([](Http::Code code, absl::string_view body,
                          std::function<void(Http::ResponseHeaderMap & headers)> modify_headers,
                          const absl::optional<Grpc::Status::GrpcStatus> grpc_status,
                          absl::string_view details) {
        EXPECT_EQ(Http::Code::TooManyRequests, code);
        EXPECT_EQ("local_rate_limited", body);

        Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
        modify_headers(response_headers);
        EXPECT_EQ("true", response_headers.get(Http::LowerCaseString("x-test-rate-limit"))[0]
                              ->value()
                              .getStringView());
        // Make sure that generated local reply headers contain a value dynamically
        // generated by header formatter REQ(test-req-id)
        EXPECT_EQ("123", response_headers.get(Http::LowerCaseString("test-resp-req-id"))[0]
                             ->value()
                             .getStringView());
        EXPECT_EQ(grpc_status,
                  absl::make_optional(Grpc::Status::WellKnownGrpcStatus::ResourceExhausted));
        EXPECT_EQ(details, "local_rate_limited");
      }));

  // Add a custom header to the request.
  // Locally generated reply is configured to refer to this value.
  Http::TestRequestHeaderMapImpl request_headers{{"test-req-id", "123"}};
  NiceMock<StreamInfo::MockStreamInfo> stream_info;

  EXPECT_CALL(decoder_callbacks_2_, streamInfo).WillRepeatedly(testing::ReturnRef(stream_info));
  EXPECT_CALL(stream_info, getRequestHeaders).WillRepeatedly(Return(&request_headers));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_2_->decodeHeaders(request_headers, false));
  EXPECT_EQ(2U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.ok"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

/*
This test sets 'local_rate_limit_per_downstream_connection' to true. Doing this enables per
connection rate limiting and even though 'max_token' is set to 1, it allows 2 requests to go through
- one on each connection. This is in contrast to the 'RequestOk' test above where only 1 request is
allowed (across the process) for the same configuration.
*/
TEST_F(FilterTest, RequestRateLimitedPerConnection) {
  setup(fmt::format(config_yaml, "false", "1", "true", "\"OFF\""));

  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::TooManyRequests, _, _, _, _))
      .WillOnce(Invoke([](Http::Code code, absl::string_view body,
                          std::function<void(Http::ResponseHeaderMap & headers)> modify_headers,
                          const absl::optional<Grpc::Status::GrpcStatus> grpc_status,
                          absl::string_view details) {
        EXPECT_EQ(Http::Code::TooManyRequests, code);
        EXPECT_EQ("local_rate_limited", body);

        Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
        modify_headers(response_headers);
        EXPECT_EQ("true", response_headers.get(Http::LowerCaseString("x-test-rate-limit"))[0]
                              ->value()
                              .getStringView());

        EXPECT_EQ(grpc_status, absl::nullopt);
        EXPECT_EQ(details, "local_rate_limited");
      }));

  auto request_headers = Http::TestRequestHeaderMapImpl();
  auto expected_headers = Http::TestRequestHeaderMapImpl();

  EXPECT_EQ(request_headers, expected_headers);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_2_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_2_->decodeHeaders(request_headers, false));
  EXPECT_EQ(4U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(2U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(2U, findCounter("test.http_local_rate_limit.ok"));
  EXPECT_EQ(2U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(FilterTest, RequestRateLimitedButNotEnforced) {
  setup(fmt::format(config_yaml, "false", "0", "false", "\"OFF\""), true, false);

  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::TooManyRequests, _, _, _, _)).Times(0);

  auto request_headers = Http::TestRequestHeaderMapImpl();
  Http::TestRequestHeaderMapImpl expected_headers{
      {"x-local-ratelimited", "true"},
  };
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(request_headers, expected_headers);
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(FilterTest, RequestRateLimitedXRateLimitHeaders) {
  setup(fmt::format(config_yaml, "false", "1", "false", "DRAFT_VERSION_03"));

  auto request_headers = Http::TestRequestHeaderMapImpl();
  auto response_headers = Http::TestResponseHeaderMapImpl();

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
  EXPECT_EQ("1", response_headers.get_("x-ratelimit-limit"));
  EXPECT_EQ("0", response_headers.get_("x-ratelimit-remaining"));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_2_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_2_->encodeHeaders(response_headers, false));
  EXPECT_EQ("1", response_headers.get_("x-ratelimit-limit"));
  EXPECT_EQ("0", response_headers.get_("x-ratelimit-remaining"));
  EXPECT_EQ(2U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.ok"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(FilterTest, RequestRateLimitedXRateLimitHeadersWithoutRunningDecodeHeaders) {
  setup(fmt::format(config_yaml, "false", "1", "false", "DRAFT_VERSION_03"));

  auto response_headers = Http::TestResponseHeaderMapImpl();

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
  EXPECT_EQ("", response_headers.get_("x-ratelimit-limit"));
  EXPECT_EQ("", response_headers.get_("x-ratelimit-remaining"));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_2_->encodeHeaders(response_headers, false));
  EXPECT_EQ("", response_headers.get_("x-ratelimit-limit"));
  EXPECT_EQ("", response_headers.get_("x-ratelimit-remaining"));
}

static constexpr absl::string_view descriptor_config_yaml = R"(
stat_prefix: test
token_bucket:
  max_tokens: {}
  tokens_per_fill: 1
  fill_interval: 60s
filter_enabled:
  runtime_key: test_enabled
  default_value:
    numerator: 100
    denominator: HUNDRED
filter_enforced:
  runtime_key: test_enforced
  default_value:
    numerator: 100
    denominator: HUNDRED
response_headers_to_add:
  - append_action: OVERWRITE_IF_EXISTS_OR_ADD
    header:
      key: x-test-rate-limit
      value: 'true'
local_rate_limit_per_downstream_connection: true
enable_x_ratelimit_headers: {}
descriptors:
- entries:
   - key: hello
     value: world
   - key: foo
     value: bar
  token_bucket:
    max_tokens: 10
    tokens_per_fill: 10
    fill_interval: 60s
- entries:
   - key: foo2
     value: bar2
  token_bucket:
    max_tokens: {}
    tokens_per_fill: 1
    fill_interval: 60s
stage: {}
  )";

static constexpr absl::string_view inlined_descriptor_config_yaml = R"(
stat_prefix: test
token_bucket:
  # Big global token bucket to ensure that the request
  # is not rate limited by the global token bucket.
  max_tokens: 100000
  tokens_per_fill: 100000
  fill_interval: 1s
filter_enabled:
  runtime_key: test_enabled
  default_value:
    numerator: 100
    denominator: HUNDRED
filter_enforced:
  runtime_key: test_enforced
  default_value:
    numerator: 100
    denominator: HUNDRED
descriptors:
- entries:
   - key: foo2
     value: bar2
  token_bucket:
    max_tokens: {}
    tokens_per_fill: {}
    fill_interval: {}
rate_limits:
- actions:
  - header_value_match:
      descriptor_key: foo2
      descriptor_value: bar2
      headers:
      - name: x-header-name
        string_match:
          exact: test_value
  )";

static constexpr absl::string_view inlined_descriptor_config_yaml_with_custom_hits_addend = R"(
stat_prefix: test
token_bucket:
  # Big global token bucket to ensure that the request
  # is not rate limited by the global token bucket.
  max_tokens: 100000
  tokens_per_fill: 100000
  fill_interval: 1s
filter_enabled:
  runtime_key: test_enabled
  default_value:
    numerator: 100
    denominator: HUNDRED
filter_enforced:
  runtime_key: test_enforced
  default_value:
    numerator: 100
    denominator: HUNDRED
descriptors:
- entries:
   - key: foo2
     value: bar2
  token_bucket:
    max_tokens: {}
    tokens_per_fill: {}
    fill_interval: {}
rate_limits:
- actions:
  - header_value_match:
      descriptor_key: foo2
      descriptor_value: bar2
      headers:
      - name: x-header-name
        string_match:
          exact: test_value_1
  hits_addend:
    format: "%BYTES_RECEIVED%"
- actions:
  - header_value_match:
      descriptor_key: foo2
      descriptor_value: bar2
      headers:
      - name: x-header-name
        string_match:
          exact: test_value_2
  hits_addend:
    number: 5
  )";

static constexpr absl::string_view consume_default_token_config_yaml = R"(
stat_prefix: test
token_bucket:
  max_tokens: {}
  tokens_per_fill: 1
  fill_interval: 60s
filter_enabled:
  runtime_key: test_enabled
  default_value:
    numerator: 100
    denominator: HUNDRED
filter_enforced:
  runtime_key: test_enforced
  default_value:
    numerator: 100
    denominator: HUNDRED
response_headers_to_add:
  - append_action: OVERWRITE_IF_EXISTS_OR_ADD
    header:
      key: x-test-rate-limit
      value: 'true'
local_rate_limit_per_downstream_connection: true
always_consume_default_token_bucket: {}
descriptors:
- entries:
   - key: hello
     value: world
   - key: foo
     value: bar
  token_bucket:
    max_tokens: 10
    tokens_per_fill: 10
    fill_interval: 60s
- entries:
   - key: foo2
     value: bar2
  token_bucket:
    max_tokens: {}
    tokens_per_fill: 1
    fill_interval: 60s
stage: {}
  )";

static constexpr absl::string_view descriptor_vh_config_yaml = R"(
stat_prefix: test
token_bucket:
  max_tokens: {}
  tokens_per_fill: 1
  fill_interval: 60s
filter_enabled:
  runtime_key: test_enabled
  default_value:
    numerator: 100
    denominator: HUNDRED
filter_enforced:
  runtime_key: test_enforced
  default_value:
    numerator: 100
    denominator: HUNDRED
response_headers_to_add:
  - append_action: OVERWRITE_IF_EXISTS_OR_ADD
    header:
      key: x-test-rate-limit
      value: 'true'
local_rate_limit_per_downstream_connection: true
enable_x_ratelimit_headers: {}
descriptors:
- entries:
   - key: hello
     value: world
   - key: foo
     value: bar
  token_bucket:
    max_tokens: 10
    tokens_per_fill: 10
    fill_interval: 60s
- entries:
   - key: foo2
     value: bar2
  token_bucket:
    max_tokens: {}
    tokens_per_fill: 1
    fill_interval: 60s
stage: {}
vh_rate_limits: {}
  )";

class DescriptorFilterTest : public FilterTest {
public:
  DescriptorFilterTest() = default;

  void setUpTest(const std::string& yaml) {
    setupPerRoute(yaml, true, true, true);
    decoder_callbacks_.route_->route_entry_.rate_limit_policy_.rate_limit_policy_entry_.clear();
    decoder_callbacks_.route_->route_entry_.rate_limit_policy_.rate_limit_policy_entry_
        .emplace_back(route_rate_limit_);
    decoder_callbacks_.route_->virtual_host_->rate_limit_policy_.rate_limit_policy_entry_.clear();
    decoder_callbacks_.route_->virtual_host_->rate_limit_policy_.rate_limit_policy_entry_
        .emplace_back(vh_rate_limit_);
  }

  std::vector<RateLimit::Descriptor> descriptor_{{{{"foo2", "bar2"}}}};
  std::vector<RateLimit::Descriptor> descriptor_first_match_{{
      {{
          {"hello", "world"},
          {"foo", "bar"},
      }},
      {{{"foo2", "bar2"}}},
  }};
  std::vector<RateLimit::Descriptor> descriptor_not_found_{{{{"foo", "bar"}}}};
  NiceMock<Router::MockRateLimitPolicyEntry> route_rate_limit_;
  NiceMock<Router::MockRateLimitPolicyEntry> vh_rate_limit_;
};

TEST_F(DescriptorFilterTest, NoRouteEntry) {
  setupPerRoute(fmt::format(descriptor_config_yaml, "1", "\"OFF\"", "1", "0"), true, true, true);

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.ok"));
}

TEST_F(DescriptorFilterTest, NoCluster) {
  setUpTest(fmt::format(descriptor_config_yaml, "1", "\"OFF\"", "1", "0"));

  EXPECT_CALL(decoder_callbacks_, clusterInfo()).WillRepeatedly(testing::Return(nullptr));

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.ok"));
}

TEST_F(DescriptorFilterTest, DisabledInRoute) {
  setUpTest(fmt::format(descriptor_config_yaml, "1", "\"OFF\"", "1", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  route_rate_limit_.disable_key_ = "disabled";

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.ok"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorRequestOk) {
  setUpTest(fmt::format(descriptor_config_yaml, "1", "\"OFF\"", "1", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.ok"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorRequestRatelimited) {
  setUpTest(fmt::format(descriptor_config_yaml, "0", "\"OFF\"", "0", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorNotFound) {
  setUpTest(fmt::format(descriptor_config_yaml, "1", "\"OFF\"", "1", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_not_found_));

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorNotFoundWithConsumeDefaultTokenTrue) {
  setUpTest(fmt::format(consume_default_token_config_yaml, "0", "true", "1", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_not_found_));

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorWithConsumeDefaultTokenTrue) {
  setUpTest(fmt::format(consume_default_token_config_yaml, "0", "true", "1", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorWithConsumeDefaultTokenFalse) {
  setUpTest(fmt::format(consume_default_token_config_yaml, "0", "false", "1", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorNotFoundWithConsumeDefaultTokenFalse) {
  setUpTest(fmt::format(consume_default_token_config_yaml, "0", "false", "1", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_not_found_));

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorBothMatch) {
  // Request should also be rate limited as it should match both descriptors and global token.
  setUpTest(fmt::format(descriptor_config_yaml, "0", "\"OFF\"", "0", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_first_match_));

  Http::TestRequestHeaderMapImpl headers;
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorWithStageConfig) {
  setUpTest(fmt::format(descriptor_config_yaml, "1", "\"OFF\"", "1", "1"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(1));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(0U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.ok"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorRequestRatelimitedXRateLimitHeaders) {
  setUpTest(fmt::format(descriptor_config_yaml, "0", "DRAFT_VERSION_03", "0", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  auto request_headers = Http::TestRequestHeaderMapImpl();
  auto response_headers = Http::TestResponseHeaderMapImpl();

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
  EXPECT_EQ("0", response_headers.get_("x-ratelimit-limit"));
  EXPECT_EQ("0", response_headers.get_("x-ratelimit-remaining"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(DescriptorFilterTest, RouteDescriptorRequestRatelimitedWithoutXRateLimitHeaders) {
  setUpTest(fmt::format(descriptor_config_yaml, "0", "\"OFF\"", "0", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  auto request_headers = Http::TestRequestHeaderMapImpl();
  auto response_headers = Http::TestResponseHeaderMapImpl();

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
  EXPECT_TRUE(response_headers.get(Http::LowerCaseString("x-ratelimit-limit")).empty());
  EXPECT_TRUE(response_headers.get(Http::LowerCaseString("x-ratelimit-remaining")).empty());
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enabled"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.enforced"));
  EXPECT_EQ(1U, findCounter("test.http_local_rate_limit.rate_limited"));
}

TEST_F(DescriptorFilterTest, NoVHRateLimitOption) {
  setUpTest(fmt::format(descriptor_config_yaml, "1", "\"OFF\"", "1", "0"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));
  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_, empty())
      .WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0))
      .Times(0);
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
}

// Tests that the route rate limit is used when VhRateLimitsOptions::OVERRIDE and route rate limit
// is set
TEST_F(DescriptorFilterTest, OverrideVHRateLimitOptionWithRouteRateLimitSet) {
  setUpTest(fmt::format(descriptor_vh_config_yaml, "1", "\"OFF\"", "1", "0", "OVERRIDE"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));
  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_, empty())
      .WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0))
      .Times(0);
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
}

// Tests that the virtual host rate limit is used when VhRateLimitsOptions::OVERRIDE is set and
// route rate limit is empty
TEST_F(DescriptorFilterTest, OverrideVHRateLimitOptionWithoutRouteRateLimit) {
  setUpTest(fmt::format(descriptor_vh_config_yaml, "1", "\"OFF\"", "1", "0", "OVERRIDE"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_, empty())
      .WillOnce(Return(true));

  EXPECT_CALL(decoder_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
}

// Tests that the virtual host rate limit is used when VhRateLimitsOptions::INCLUDE is set and route
// rate limit is empty
TEST_F(DescriptorFilterTest, IncludeVHRateLimitOptionWithOnlyVHRateLimitSet) {
  setUpTest(fmt::format(descriptor_vh_config_yaml, "1", "\"OFF\"", "1", "0", "INCLUDE"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
}

// Tests that the virtual host rate limit is used when VhRateLimitsOptions::INCLUDE and route rate
// limit is set
TEST_F(DescriptorFilterTest, IncludeVHRateLimitOptionWithRouteAndVHRateLimitSet) {
  setUpTest(fmt::format(descriptor_vh_config_yaml, "1", "\"OFF\"", "1", "0", "INCLUDE"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
}

// Tests that the route rate limit is used when VhRateLimitsOptions::IGNORE and route rate limit is
// set
TEST_F(DescriptorFilterTest, IgnoreVHRateLimitOptionWithRouteRateLimitSet) {
  setUpTest(fmt::format(descriptor_vh_config_yaml, "1", "\"OFF\"", "1", "0", "IGNORE"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0))
      .Times(0);

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
}

// Tests that no rate limit is used when VhRateLimitsOptions::IGNORE is set and route rate limit
// empty
TEST_F(DescriptorFilterTest, IgnoreVHRateLimitOptionWithOutRouteRateLimit) {
  setUpTest(fmt::format(descriptor_vh_config_yaml, "1", "\"OFF\"", "1", "0", "IGNORE"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(decoder_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0))
      .Times(0);

  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
}

// Tests that the virtual host rate limit is used when includeVirtualHostRateLimits is used
TEST_F(DescriptorFilterTest, IncludeVirtualHostRateLimitsSetTrue) {
  setUpTest(fmt::format(descriptor_vh_config_yaml, "1", "\"OFF\"", "1", "0", "IGNORE"));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_.rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(decoder_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(true));

  EXPECT_CALL(decoder_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(testing::SetArgReferee<0>(descriptor_));
  auto headers = Http::TestRequestHeaderMapImpl();
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
}

TEST_F(DescriptorFilterTest, UseInlinedRateLimitConfig) {
  setUpTest(fmt::format(inlined_descriptor_config_yaml, 1, 1, "60s"));

  auto headers = Http::TestRequestHeaderMapImpl();
  // Requests will not be blocked because the requests don't match any descriptor and
  // the global token bucket has enough tokens.
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));

  headers.setCopy(Http::LowerCaseString("x-header-name"), "test_value");

  // Only one request is allowed in 60s for the matched request.
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::TooManyRequests, _, _, _, _));
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration, filter_->decodeHeaders(headers, false));
}

TEST_F(DescriptorFilterTest, UseInlinedRateLimitConfigWithCustomHitsAddend) {
  setUpTest(fmt::format(inlined_descriptor_config_yaml_with_custom_hits_addend, 119, 119, "60s"));

  auto headers = Http::TestRequestHeaderMapImpl();

  // Requests will not be blocked because the requests don't match any descriptor and
  // the global token bucket has enough tokens.
  for (size_t i = 0; i < 120; i++) {
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  }

  decoder_callbacks_.stream_info_.bytes_received_ = 100;

  {
    headers.setCopy(Http::LowerCaseString("x-header-name"), "test_value_1");

    // 119 -> 19.
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));
  }

  {
    headers.setCopy(Http::LowerCaseString("x-header-name"), "test_value_2");

    // 19 -> 14.
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));

    // 14 -> 9.
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));

    // 9 -> 4.
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(headers, false));

    // No enough tokens.
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::TooManyRequests, _, _, _, _));
    EXPECT_EQ(Http::FilterHeadersStatus::StopIteration, filter_->decodeHeaders(headers, false));
  }
}

} // namespace LocalRateLimitFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
