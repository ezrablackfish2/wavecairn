#include "wavecairn/command_builder.h"
#include "wavecairn/package.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

void write_file(const std::string& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string tape_path = argc > 1 ? argv[1] : "demo.wctp";
    const std::string package_path = argc > 2 ? argv[2] : "demo.wcpk";

    using wavecairn::EntityId;
    wavecairn::CommandBuilder builder(42);
    const auto media = EntityId::from_name("media", "dialogue-take-01");
    const auto track = EntityId::from_name("track", "dialogue");
    const auto clip = EntityId::from_name("clip", "dialogue-opening");
    const auto marker = EntityId::from_name("marker", "opening");
    const auto plugin = EntityId::from_name("plugin", "dialogue-eq");
    const auto render = EntityId::from_name("render", "dialogue-stem");

    builder.add_media(media,
                      wavecairn::MediaKind::audio,
                      "Dialogue take 01",
                      "Media/dialogue_take_01.wav",
                      480000,
                      0x51a72c0bULL,
                      48000,
                      1)
        .barrier()
        .add_track(track, "Dialogue", 1, 0.9F, 0.0F)
        .add_clip(clip, track, media, 0, 0, 384000)
        .add_marker(marker, 0, 48000, "Opening line", "First spoken cue", 0x4a90e2)
        .barrier()
        .add_plugin(plugin, track, "WaveCairn", "Reference EQ",
                    std::array<float, 3>{0.0F, -2.5F, 1.0F}, 64)
        .add_render_job(render, track, 0, 384000, 48000, 1, 10, 0x01U)
        .partition_render(4, 96000)
        .add_project_record(EntityId::from_name("project", "delivery"),
                            "delivery.profile",
                            "dialogue-stem-48k-mono")
        .barrier();

    const auto tape = builder.serialize();
    write_file(tape_path, tape);

    wavecairn::PackageDocument package;
    package.version = 1;
    package.package_id = 42;
    package.sections.push_back(wavecairn::PackageSection{
        wavecairn::make_section_tag("TAPE"), 0, 1, tape});
    const std::string metadata =
        "title=WaveCairn demonstration\ncreator=WaveCairn example\napplication=make_demo\n";
    package.sections.push_back(wavecairn::PackageSection{
        wavecairn::make_section_tag("META"),
        0,
        1,
        std::vector<std::uint8_t>(metadata.begin(), metadata.end())});
    const auto package_bytes = wavecairn::serialize_package(package);
    write_file(package_path, package_bytes);

    std::cout << "wrote " << tape_path << " and " << package_path << '\n';
    return 0;
}
