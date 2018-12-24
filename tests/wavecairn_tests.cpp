#include "wavecairn/analysis.h"
#include "wavecairn/command_builder.h"
#include "wavecairn/manifest.h"
#include "wavecairn/merge.h"
#include "wavecairn/package.h"
#include "wavecairn/path_mapper.h"
#include "wavecairn/report.h"
#include "wavecairn/session.h"
#include "wavecairn/timecode.h"
#include "wavecairn/tape.h"
#include "wavecairn/validator.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

wavecairn::CommandBuilder demo_builder(std::uint32_t seed = 7) {
    using wavecairn::EntityId;
    wavecairn::CommandBuilder builder(seed);
    const auto media = EntityId::from_name("media", "voice");
    const auto track = EntityId::from_name("track", "voice");
    const auto clip = EntityId::from_name("clip", "voice-1");
    const auto lane = EntityId::from_name("lane", "voice-volume");
    const auto marker = EntityId::from_name("marker", "start");
    const auto take = EntityId::from_name("take", "voice-alt");
    const auto plugin = EntityId::from_name("plugin", "voice-eq");
    const auto render = EntityId::from_name("render", "voice-stem");
    const std::array<std::int32_t, 3> samples{0, 24000, 48000};
    const std::array<float, 3> values{0.7F, 1.0F, 0.8F};
    const std::array<float, 2> parameters{0.25F, -1.0F};

    builder.add_media(media,
                      wavecairn::MediaKind::audio,
                      "Voice",
                      "Media/voice.wav",
                      96000,
                      0x12345678ULL,
                      48000,
                      1)
        .add_track(track, "Voice", 1, 1.0F, 0.0F)
        .add_clip(clip, track, media, 0, 0, 72000)
        .add_tempo(EntityId::from_name("tempo", "main"), 0, 120.0, 120.0)
        .add_automation_lane(lane, track, "volume", 1.0F)
        .add_automation_points(lane, samples, values)
        .add_marker(marker, 0, 12000, "Start", "Opening")
        .add_take(take, track, media, 12000, 48000, 1, 4.5F, 0x01U)
        .add_plugin(plugin, track, "WaveCairn", "EQ", parameters, 32)
        .add_render_job(render, track, 0, 72000, 48000, 1, 1, 0)
        .partition_render(2, 24000)
        .add_project_record(EntityId::from_name("record", "owner"),
                            "project.owner",
                            "post-production")
        .barrier();
    return builder;
}

void test_tape_roundtrip() {
    const auto builder = demo_builder();
    const auto bytes = builder.serialize();
    wavecairn::Diagnostics diagnostics;
    const auto parsed = wavecairn::parse_command_tape(bytes, &diagnostics);
    expect(parsed.ok(), "command tape parses");
    if (parsed) {
        expect(parsed.value().commands.size() == builder.tape().commands.size(),
               "command count survives roundtrip");
        const auto second = wavecairn::serialize_command_tape(parsed.value());
        expect(second == bytes, "command tape serialization is deterministic");
    }
}

void test_session_processing() {
    auto builder = demo_builder();
    auto bytes = builder.serialize();
    wavecairn::Session session;
    wavecairn::Diagnostics diagnostics;
    wavecairn::ProcessingOptions options;
    options.canonicalize_at_end = true;
    const auto result = wavecairn::process_input(session, bytes, diagnostics, options);
    expect(result.ok(), "session processing succeeds");
    expect(session.media().asset_count() == 1, "media asset imported");
    expect(session.timeline().tracks().size() == 1, "track imported");
    expect(session.timeline().clips().size() == 1, "clip imported");
    expect(session.routing().plugins().size() == 1, "plugin imported");
    expect(session.rendering().jobs().size() == 1, "render job imported");
    expect(session.timeline().end_sample() == 72000, "timeline end calculated");

    wavecairn::SessionValidator validator;
    wavecairn::Diagnostics validation;
    const auto summary = validator.validate(session, validation);
    expect(summary.errors == 0, "valid session has no validation errors");
}

void test_package_roundtrip() {
    const auto tape = demo_builder().serialize();
    wavecairn::PackageDocument package;
    package.package_id = 99;
    package.sections.push_back({wavecairn::make_section_tag("TAPE"), 0, 1, tape});
    const std::string metadata = "title=Test package\ncreator=Unit test\n";
    package.sections.push_back({wavecairn::make_section_tag("META"), 0, 1,
        std::vector<std::uint8_t>(metadata.begin(), metadata.end())});
    const auto bytes = wavecairn::serialize_package(package);
    wavecairn::Diagnostics diagnostics;
    const auto parsed = wavecairn::parse_package(bytes, &diagnostics);
    expect(parsed.ok(), "package parses");
    if (parsed) {
        expect(parsed.value().sections.size() == 2, "package section count survives");
        expect(wavecairn::serialize_package(parsed.value()) == bytes,
               "package serialization is deterministic");
    }

    wavecairn::Session session;
    wavecairn::ProcessingOptions options;
    options.canonicalize_at_end = true;
    const auto processed = wavecairn::process_package(session, bytes, diagnostics, options);
    expect(processed.ok(), "package command section processes");
    expect(session.timeline().tracks().size() == 1, "package created timeline");
}

void test_manifest_and_reports() {
    const auto bytes = demo_builder().serialize();
    wavecairn::Session session;
    wavecairn::Diagnostics diagnostics;
    expect(wavecairn::process_input(session, bytes, diagnostics).ok(),
           "report session imports");
    const auto manifest = wavecairn::build_manifest(session);
    const auto text = wavecairn::serialize_manifest(manifest);
    const auto parsed = wavecairn::parse_manifest(text);
    expect(parsed.ok(), "manifest parses");
    expect(!manifest.entries.empty(), "manifest contains entries");

    wavecairn::ReportOptions options;
    options.include_manifest = true;
    options.include_occupancy = true;
    const auto report = wavecairn::build_report(session, &diagnostics, options);
    expect(wavecairn::report_as_text(report).find("Session overview") != std::string::npos,
           "text report contains overview");
    expect(wavecairn::report_as_markdown(report).find("# ") == 0,
           "markdown report has heading");
    expect(wavecairn::report_as_json(report).find("\"sections\"") != std::string::npos,
           "json report has sections");
    expect(wavecairn::report_as_csv(report).find("# Session overview") != std::string::npos,
           "csv report has section marker");
}

void test_path_mapping() {
    const auto bytes = demo_builder().serialize();
    wavecairn::Session session;
    wavecairn::Diagnostics diagnostics;
    expect(wavecairn::process_input(session, bytes, diagnostics).ok(),
           "path mapping session imports");
    wavecairn::PathMapper mapper;
    mapper.add_rule({wavecairn::PathRuleKind::directory,
                     "Media", "Assets", false, true});
    const auto plan = mapper.plan(session);
    expect(plan.size() == 1, "path mapping has one media row");
    expect(plan.front().mapped == "Assets/voice.wav", "path rule maps directory");
    expect(mapper.apply(session, diagnostics) == 1, "path mapping applies one change");
}

void test_timecode() {
    const auto rate = wavecairn::TimecodeRate::parse("30000/1001");
    expect(rate.has_value(), "timecode rate parses");
    if (!rate) {
        return;
    }
    const auto value = wavecairn::Timecode::parse("01:02:03:12", *rate);
    expect(value.has_value(), "timecode value parses");
    if (value) {
        const auto frames = wavecairn::timecode_to_frames(*value, *rate);
        const auto roundtrip = wavecairn::frames_to_timecode(frames, *rate);
        expect(roundtrip.hours == value->hours, "timecode hours roundtrip");
        expect(roundtrip.minutes == value->minutes, "timecode minutes roundtrip");
    }
}

void test_analysis_and_merge() {
    auto base_bytes = demo_builder(10).serialize();
    auto incoming_builder = demo_builder(11);
    const auto second_track = wavecairn::EntityId::from_name("track", "music");
    incoming_builder.add_track(second_track, "Music", 2, 0.8F, 0.0F);
    auto incoming_bytes = incoming_builder.serialize();

    wavecairn::Session base;
    wavecairn::Session incoming;
    wavecairn::Diagnostics diagnostics;
    expect(wavecairn::process_input(base, base_bytes, diagnostics).ok(),
           "base session imports");
    expect(wavecairn::process_input(incoming, incoming_bytes, diagnostics).ok(),
           "incoming session imports");

    const auto graph = wavecairn::build_dependency_graph(incoming);
    expect(!graph.empty(), "dependency graph contains nodes");
    const auto occupancy = wavecairn::build_occupancy_map(incoming, 24000);
    expect(!occupancy.empty(), "occupancy map contains buckets");
    const auto estimate = wavecairn::estimate_render_cost(incoming);
    expect(estimate.total_frames > 0, "render estimate counts frames");

    const auto plan = wavecairn::plan_merge(base, incoming);
    expect(plan.additions >= 1, "merge plan contains addition");
    const auto result = wavecairn::apply_merge(base, plan, diagnostics);
    expect(result.ok(), "merge plan applies");
    expect(base.timeline().find_track(second_track) != nullptr,
           "merge added second track");
}

}  // namespace

int main() {
    test_tape_roundtrip();
    test_session_processing();
    test_package_roundtrip();
    test_manifest_and_reports();
    test_path_mapping();
    test_timecode();
    test_analysis_and_merge();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all WaveCairn tests passed\n";
    return EXIT_SUCCESS;
}
