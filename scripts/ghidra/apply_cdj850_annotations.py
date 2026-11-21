#!/usr/bin/env python3
"""Apply the bundled CDJ-850 annotations to one locally imported Ghidra program."""

import argparse
import json
from pathlib import Path

import pyghidra


def apply_annotations(program, annotations):
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.program.model.listing import CommentType
    from ghidra.program.model.symbol import SourceType

    def mapped_address(value):
        address = program.getAddressFactory().getAddress(value)
        if address is None or not program.getMemory().contains(address):
            raise ValueError(f"Annotation address {value} is outside mapped memory")
        return address

    listing = program.getListing()
    manager = program.getFunctionManager()
    with pyghidra.transaction(program, "Apply CDJ-850 annotations"):
        for entry in annotations["functions"]:
            address = mapped_address(entry["address"])
            function = manager.getFunctionAt(address)
            if function is None:
                if listing.getInstructionAt(address) is None:
                    command = DisassembleCommand(address, None, True)
                    if not command.applyTo(program):
                        raise ValueError(f"Could not disassemble at {address}: {command.getStatusMsg()}")
                command = CreateFunctionCmd(address)
                if not command.applyTo(program):
                    raise ValueError(f"Could not create function at {address}: {command.getStatusMsg()}")
                function = manager.getFunctionAt(address)
            if (
                function.getName() != entry["name"]
                or function.getSymbol().getSource() != SourceType.USER_DEFINED
            ):
                function.setName(entry["name"], SourceType.USER_DEFINED)

        for entry in annotations["comments"]:
            address = mapped_address(entry["address"])
            kind = CommentType.valueOf(entry["type"])
            if listing.getComment(kind, address) != entry["text"]:
                listing.setComment(address, kind, entry["text"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path, help="path to the local .gpr project file")
    parser.add_argument("program", help="program path inside the project, e.g. /main_unpacked.bin")
    args = parser.parse_args()
    if args.project.suffix != ".gpr" or not args.project.is_file():
        parser.error("project must be an existing .gpr file")

    bundle = Path(__file__).with_name("cdj850-annotations.json")
    annotations = json.loads(bundle.read_text(encoding="utf-8"))
    if annotations["version"] != 1:
        parser.error("unsupported annotation format")

    pyghidra.start()
    project_file = args.project.resolve()
    try:
        with pyghidra.open_project(project_file.parent, project_file.stem) as project:
            with pyghidra.program_context(project, "/" + args.program.lstrip("/")) as program:
                identity = {
                    "sha256": str(program.getExecutableSHA256()),
                    "language": str(program.getLanguageID()),
                    "compiler": str(program.getCompilerSpec().getCompilerSpecID()),
                    "load_address": str(program.getMemory().getMinAddress()),
                }
                for name, entry in annotations["programs"].items():
                    if all(entry[key] == value for key, value in identity.items()):
                        break
                else:
                    raise ValueError(
                        "No annotation set matches this import's SHA-256, language, "
                        "compiler spec and load address"
                    )

                apply_annotations(program, entry)
                if program.isChanged():
                    from ghidra.util.task import TaskMonitor

                    program.getDomainFile().save(TaskMonitor.DUMMY)
    except (FileNotFoundError, ValueError) as error:
        parser.exit(1, f"{error}\n")

    print(f"{name}: applied {len(entry['functions'])} function names "
          f"and {len(entry['comments'])} comments to {args.program}.")


if __name__ == "__main__":
    main()
