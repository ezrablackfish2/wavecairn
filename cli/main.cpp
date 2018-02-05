#include "wavecairn/analysis.h"
#include "wavecairn/format_registry.h"
#include "wavecairn/manifest.h"
#include "wavecairn/package.h"
#include "wavecairn/report.h"
#include "wavecairn/session.h"
#include "wavecairn/validator.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using wavecairn::ReportFormat;

std::vector<std::uint8_t> read_binary(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open input file: " + path.string());
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine input size: " + path.string());
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input && !bytes.empty()) {
        throw std::runtime_error("cannot read input file: " + path.string());
    }
    return bytes;
}

void write_binary(const std::filesystem::path& path,
                  std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open output file: " + path.string());
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    if (!output) {
        throw std::runtime_error("cannot write output file: " + path.string());
    }
}

ReportFormat parse_format(std::string_view value) {
    if (value == "text") {
        return ReportFormat::text;
    }
    if (value == "markdown" || value == "md") {
        return ReportFormat::markdown;
    }
    if (value == "json") {
        return ReportFormat::json;
    }
    if (value == "csv") {
        return ReportFormat::csv;
    }
    throw std::runtime_error("unknown report format: " + std::string(value));
}

std::optional<std::string_view> option_value(int argc,
                                             char** argv,
                                             std::string_view option) {
    for (int index = 0; index + 1 < argc; ++index) {
        if (argv[index] == option) {
            return std::string_view(argv[index + 1]);
        }
    }
    return std::nullopt;
}

void load_session(const std::filesystem::path& path,
                  wavecairn::Session& session,
                  wavecairn::Diagnostics& diagnostics) {
    auto bytes = read_binary(path);
    wavecairn::ProcessingOptions options;
    options.profile = wavecairn::ProcessingProfile::complete;
    options.canonicalize_at_end = true;
    options.maximum_commands = 65536;
    auto result = wavecairn::process_input(session, bytes, diagnostics, options);
    if (!result) {
        throw std::runtime_error(result.status().message());
    }
}

void usage(std::ostream& out) {
    out << "WaveCairn audio-session inspection toolkit\n\n"
        << "Usage:\n"
        << "  wavecairn inspect <input> [--format text|markdown|json|csv]\n"
        << "  wavecairn validate <input>\n"
        << "  wavecairn manifest <input>\n"
        << "  wavecairn graph <input>\n"
        << "  wavecairn occupancy <input> [--bucket samples]\n"
        << "  wavecairn estimate <input>\n"
        << "  wavecairn package-inventory <input.wcpk>\n"
        << "  wavecairn canonicalize-package <input.wcpk> <output.wcpk>\n"
        << "  wavecairn opcodes\n";
}

int command_inspect(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("inspect requires an input file");
    }
    wavecairn::Diagnostics diagnostics;
    wavecairn::Session session;
    load_session(argv[2], session, diagnostics);
    wavecairn::ReportOptions options;
    options.include_manifest = true;
    options.include_dependency_graph = false;
    options.format = option_value(argc, argv, "--format")
                         ? parse_format(*option_value(argc, argv, "--format"))
                         : ReportFormat::text;
    const auto report = wavecairn::build_report(session, &diagnostics, options);
    std::cout << wavecairn::render_report(report, options.format);
    return diagnostics.has_errors() ? 2 : 0;
}

int command_validate(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("validate requires an input file");
    }
    wavecairn::Diagnostics diagnostics;
    wavecairn::Session session;
    load_session(argv[2], session, diagnostics);
    wavecairn::SessionValidator validator;
    const auto summary = validator.validate(session, diagnostics);
    std::cout << diagnostics.to_text();
    std::cout << "checks=" << summary.checks
              << " warnings=" << summary.warnings
              << " errors=" << summary.errors
              << " usable=" << (summary.usable ? "yes" : "no") << '\n';
    return summary.errors == 0 ? 0 : 2;
}

int command_manifest(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("manifest requires an input file");
    }
    wavecairn::Diagnostics diagnostics;
    wavecairn::Session session;
    load_session(argv[2], session, diagnostics);
    std::cout << wavecairn::serialize_manifest(wavecairn::build_manifest(session));
    return 0;
}

int command_graph(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("graph requires an input file");
    }
    wavecairn::Diagnostics diagnostics;
    wavecairn::Session session;
    load_session(argv[2], session, diagnostics);
    std::cout << wavecairn::dependency_dot(wavecairn::build_dependency_graph(session));
    return 0;
}

int command_occupancy(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("occupancy requires an input file");
    }
    std::int64_t bucket = 48000;
    if (const auto value = option_value(argc, argv, "--bucket")) {
        bucket = std::stoll(std::string(*value));
    }
    if (bucket <= 0) {
        throw std::runtime_error("bucket size must be positive");
    }
    wavecairn::Diagnostics diagnostics;
    wavecairn::Session session;
    load_session(argv[2], session, diagnostics);
    std::cout << wavecairn::occupancy_csv(
        wavecairn::build_occupancy_map(session, bucket));
    return 0;
}

int command_estimate(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("estimate requires an input file");
    }
    wavecairn::Diagnostics diagnostics;
    wavecairn::Session session;
    load_session(argv[2], session, diagnostics);
    std::cout << wavecairn::render_estimate_text(
        wavecairn::estimate_render_cost(session));
    return 0;
}

int command_package_inventory(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("package-inventory requires an input file");
    }
    const auto bytes = read_binary(argv[2]);
    wavecairn::Diagnostics diagnostics;
    auto parsed = wavecairn::parse_package(bytes, &diagnostics);
    if (!parsed) {
        throw std::runtime_error(parsed.status().message());
    }
    wavecairn::PackageWorkspace workspace;
    auto status = workspace.load(std::move(parsed).value(), diagnostics);
    if (!status) {
        throw std::runtime_error(status.message());
    }
    std::cout << workspace.inventory_text();
    if (!diagnostics.empty()) {
        std::cerr << diagnostics.to_text();
    }
    return 0;
}

int command_canonicalize_package(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("canonicalize-package requires input and output files");
    }
    const auto bytes = read_binary(argv[2]);
    wavecairn::Diagnostics diagnostics;
    auto parsed = wavecairn::parse_package(bytes, &diagnostics);
    if (!parsed) {
        throw std::runtime_error(parsed.status().message());
    }
    wavecairn::PackageWorkspace workspace;
    auto status = workspace.load(std::move(parsed).value(), diagnostics);
    if (!status) {
        throw std::runtime_error(status.message());
    }
    workspace.canonicalize();
    const auto output = wavecairn::serialize_package(workspace.document());
    write_binary(argv[3], output);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string_view(argv[1]) == "help" ||
            std::string_view(argv[1]) == "--help") {
            usage(std::cout);
            return argc < 2 ? 1 : 0;
        }
        const std::string_view command = argv[1];
        if (command == "inspect") {
            return command_inspect(argc, argv);
        }
        if (command == "validate") {
            return command_validate(argc, argv);
        }
        if (command == "manifest") {
            return command_manifest(argc, argv);
        }
        if (command == "graph") {
            return command_graph(argc, argv);
        }
        if (command == "occupancy") {
            return command_occupancy(argc, argv);
        }
        if (command == "estimate") {
            return command_estimate(argc, argv);
        }
        if (command == "package-inventory") {
            return command_package_inventory(argc, argv);
        }
        if (command == "canonicalize-package") {
            return command_canonicalize_package(argc, argv);
        }
        if (command == "opcodes") {
            std::cout << wavecairn::format_opcode_table();
            return 0;
        }
        usage(std::cerr);
        std::cerr << "\nunknown command: " << command << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "wavecairn: " << error.what() << '\n';
        return 2;
    }
}
