#!/usr/bin/env python3
"""Generate the package-gated SolarOS manual registry and release catalog."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import tomllib
import zipfile


ID_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]*$")
FRONT_MATTER_DELIMITER = "+++"
QUICK_REFERENCE_HEADING = "quick reference"
CATALOG_SCHEMA_VERSION = 2
ARCHIVE_PATH = "manual.zip"
RELEASE_PAGE_MAX = 128 * 1024
AGENT_REFERENCE_CHUNK_MAX = 900
COMMAND_GITHUB_HREFS = {
    "agent": "agent.md",
    "expansion": "expansion.md",
    "help": "help.md",
    "identity": "identity.md",
    "job": "jobs.md",
    "jobs": "jobs.md",
    "link": "link.md",
}
SECTION_INFO = {
    "concept": (10, "Getting started"),
    "shell": (20, "Shell and storage"),
    "command": (25, "Commands"),
    "app": (30, "Applications"),
    "job": (40, "Background jobs"),
    "network": (50, "Networking and security"),
    "hardware": (60, "Hardware and expansion"),
    "api": (70, "Scripting APIs"),
    "service": (80, "System services"),
    "build": (90, "Boards and firmware"),
}


def topic_slug(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", value.casefold()).strip("-")


def package_macro(package: str) -> str:
    return "SOLAR_OS_PACKAGE_" + re.sub(r"[^A-Za-z0-9]", "_", package).upper()


def registry_conditions(
    path: Path, array_declaration: str, entry_pattern: str
) -> dict[str, str]:
    """Read the actual preprocessor gate for each registry entry."""
    conditions: dict[str, str] = {}
    stack: list[str] = []
    inside = False
    closed = False
    pattern = re.compile(entry_pattern)
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not inside:
            inside = array_declaration in line
            continue
        if line == "};":
            closed = True
            break
        if line.startswith("#if "):
            stack.append(line[4:].strip())
            continue
        if line == "#endif":
            if not stack:
                raise ValueError(f"{path}: unmatched #endif in {array_declaration}")
            stack.pop()
            continue
        if line == "#else" or line.startswith(("#ifdef ", "#ifndef ", "#elif ")):
            raise ValueError(
                f"{path}: unsupported conditional {line!r} in {array_declaration}"
            )
        match = pattern.search(line)
        if match is not None:
            name = match.group(1)
            if name in conditions:
                raise ValueError(f"{path}: duplicate registry entry {name}")
            conditions[name] = " && ".join(f"({item})" for item in stack)
    if not inside or not closed or stack:
        raise ValueError(f"{path}: malformed registry {array_declaration}")
    return conditions


def condition_packages(condition: str, known_packages: set[str]) -> list[str]:
    packages: list[str] = []
    for suffix in re.findall(r"SOLAR_OS_PACKAGE_([A-Z0-9_]+)", condition):
        package = suffix.casefold()
        if package in known_packages and package not in packages:
            packages.append(package)
    return packages


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def parse_front_matter(path: Path) -> tuple[dict[str, object], str]:
    source = path.read_text(encoding="utf-8")
    lines = source.splitlines()
    if not lines or lines[0].strip() != FRONT_MATTER_DELIMITER:
        raise ValueError(f"{path}: missing opening {FRONT_MATTER_DELIMITER}")
    try:
        end = next(
            index
            for index, line in enumerate(lines[1:], start=1)
            if line.strip() == FRONT_MATTER_DELIMITER
        )
    except StopIteration as exc:
        raise ValueError(
            f"{path}: missing closing {FRONT_MATTER_DELIMITER}"
        ) from exc
    metadata = tomllib.loads("\n".join(lines[1:end]))
    markdown = "\n".join(lines[end + 1 :]).strip()
    if not markdown:
        raise ValueError(f"{path}: Markdown body is empty")
    return metadata, markdown + "\n"


def heading_text(line: str) -> tuple[int, str] | None:
    match = re.match(r"^(#{1,6})\s+(.+?)\s*$", line)
    if match is None:
        return None
    return len(match.group(1)), match.group(2).strip()


def extract_quick_reference(markdown: str, path: Path) -> str:
    lines = markdown.splitlines()
    start = None
    level = 0
    for index, line in enumerate(lines):
        heading = heading_text(line)
        if heading is None or heading[1].casefold() != QUICK_REFERENCE_HEADING:
            continue
        start = index + 1
        level = heading[0]
        break
    if start is None:
        raise ValueError(f"{path}: missing Quick reference heading")

    end = len(lines)
    for index in range(start, len(lines)):
        heading = heading_text(lines[index])
        if heading is not None and heading[0] <= level:
            end = index
            break
    reference = "\n".join(lines[start:end]).strip()
    if not reference:
        raise ValueError(f"{path}: Quick reference section is empty")
    return reference


def strip_inline_markdown(text: str) -> str:
    text = re.sub(r"!\[([^]]*)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"\[([^]]+)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"`([^`]+)`", r"\1", text)
    text = text.replace("**", "").replace("__", "")
    text = text.replace("*", "")
    return text.strip()


def first_paragraph(markdown: str) -> str:
    paragraph: list[str] = []
    for line in markdown.splitlines():
        stripped = line.strip()
        if not stripped:
            if paragraph:
                break
            continue
        if stripped.startswith(("#", "```", "|", "-", "*", ">")):
            if paragraph:
                break
            continue
        paragraph.append(strip_inline_markdown(stripped))
    return " ".join(paragraph).strip()


def markdown_section(markdown: str, heading: str) -> str | None:
    lines = markdown.splitlines()
    start = None
    for index, line in enumerate(lines):
        parsed = heading_text(line)
        if parsed == (2, heading):
            start = index + 1
            break
    if start is None:
        return None
    end = len(lines)
    for index in range(start, len(lines)):
        parsed = heading_text(lines[index])
        if parsed is not None and parsed[0] <= 2:
            end = index
            break
    return "\n".join(lines[start:end]).strip()


def toml_array(values: list[str]) -> str:
    return "[" + ", ".join(json.dumps(value) for value in values) + "]"


def release_markdown(page: dict[str, object]) -> str:
    lines = [
        FRONT_MATTER_DELIMITER,
        f"id = {json.dumps(str(page['id']))}",
        f"title = {json.dumps(str(page['title']))}",
        f"section = {json.dumps(str(page['section']))}",
        f"summary = {json.dumps(str(page['summary']))}",
        f"aliases = {toml_array(list(page['aliases']))}",
        f"keywords = {json.dumps(str(page['keywords']))}",
        f"packages_any = {toml_array(list(page['packages_any']))}",
        FRONT_MATTER_DELIMITER,
        str(page["markdown"]).rstrip(),
        "",
    ]
    return "\n".join(lines)


def derived_page(
    *,
    page_id: str,
    title: str,
    section: str,
    summary: str,
    alias: str,
    keywords: str,
    packages_any: list[str],
    markdown: str,
    contract: str,
    source_path: Path,
    source_href: str,
    condition: str = "",
) -> dict[str, object]:
    return {
        "id": page_id,
        "title": title,
        "section": section,
        "section_title": SECTION_INFO[section][1],
        "summary": summary,
        "aliases": [alias],
        "keywords": keywords,
        "packages_any": packages_any,
        "path": source_path,
        "github_href": source_href,
        "markdown": markdown.rstrip() + "\n",
        "body": markdown_to_terminal_text(markdown),
        "contract": contract.strip(),
        "derived": True,
        "condition": condition,
    }


def derive_application_pages(
    source: Path,
    pages: list[dict[str, object]],
    known_packages: set[str],
    conditions: dict[str, str],
) -> list[dict[str, object]]:
    path = source / "apps.md"
    page = next(item for item in pages if item["id"] == "apps")
    markdown = str(page["markdown"])
    derived: list[dict[str, object]] = []
    documented: set[str] = set()
    for match in re.finditer(r"^## ([a-z0-9-]+)\s*$", markdown, re.MULTILINE):
        name = match.group(1)
        content = markdown_section(markdown, name)
        if content is None:
            continue
        if name not in conditions:
            raise ValueError(f"apps.md documents unregistered application {name}")
        documented.add(name)
        condition = conditions[name]
        packages_any = condition_packages(condition, known_packages)
        summary = first_paragraph(content) or f"Use the {name} application"
        body = f"# {name}\n\n{content}\n"
        derived.append(
            derived_page(
                page_id=f"app.{name}",
                title=f"{name} application",
                section="app",
                summary=summary,
                alias=name,
                keywords=f"{name} app application usage controls examples",
                packages_any=packages_any,
                markdown=body,
                contract=content,
                source_path=path,
                source_href=f"apps.md#{topic_slug(name)}",
                condition=condition,
            )
        )
    missing = sorted(set(conditions) - documented)
    if missing:
        raise ValueError(f"apps.md omits registered applications: {', '.join(missing)}")
    return derived


def derive_job_pages(
    source: Path,
    pages: list[dict[str, object]],
    known_packages: set[str],
) -> list[dict[str, object]]:
    path = source / "jobs.reference.md"
    page = next(item for item in pages if item["id"] == "jobs.reference")
    markdown = str(page["markdown"])
    derived: list[dict[str, object]] = []
    for match in re.finditer(r"^## ([a-z0-9-]+)\s*$", markdown, re.MULTILINE):
        name = match.group(1)
        content = markdown_section(markdown, name)
        if content is None:
            continue
        package = "job_" + name.replace("-", "_")
        packages_any = [package] if package in known_packages else []
        summary = first_paragraph(content) or f"Run the {name} background job"
        body = f"# {name}\n\n{content}\n"
        derived.append(
            derived_page(
                page_id=f"job.{name}",
                title=f"{name} job",
                section="job",
                summary=summary,
                alias=name,
                keywords=f"{name} job background start stop status examples",
                packages_any=packages_any,
                markdown=body,
                contract=content,
                source_path=path,
                source_href=f"jobs.reference.md#{topic_slug(name)}",
            )
        )
    return derived


def split_table_row(line: str) -> list[str]:
    source = line.strip()
    if source.startswith("|"):
        source = source[1:]
    if source.endswith("|"):
        source = source[:-1]
    cells: list[str] = []
    cell: list[str] = []
    inline_code = False
    escaped = False
    for char in source:
        if escaped:
            cell.append(char)
            escaped = False
            continue
        if char == "\\":
            cell.append(char)
            escaped = True
            continue
        if char == "`":
            inline_code = not inline_code
            cell.append(char)
            continue
        if char == "|" and not inline_code:
            cells.append("".join(cell).strip())
            cell.clear()
            continue
        cell.append(char)
    cells.append("".join(cell).strip())
    return cells


def derive_command_pages(
    source: Path,
    pages: list[dict[str, object]],
    known_packages: set[str],
    conditions: dict[str, str],
) -> list[dict[str, object]]:
    path = source / "commands.md"
    page = next(item for item in pages if item["id"] == "commands")
    lines = str(page["markdown"]).splitlines()
    commands: dict[str, list[tuple[str, str]]] = {}
    index = 0
    while index + 2 < len(lines):
        header = split_table_row(lines[index]) if lines[index].startswith("|") else []
        if header != ["Command", "Usage", "Description"]:
            index += 1
            continue
        index += 2
        while index < len(lines) and lines[index].startswith("|"):
            cells = split_table_row(lines[index])
            if len(cells) == 3:
                command = strip_inline_markdown(cells[0]).split()[0]
                usage = cells[1]
                description = cells[2]
                commands.setdefault(command, []).append((usage, description))
            index += 1

    derived: list[dict[str, object]] = []
    for name, rows in commands.items():
        if name not in conditions:
            raise ValueError(f"commands.md documents unregistered command {name}")
        condition = conditions[name]
        summary = strip_inline_markdown(rows[0][1])
        table = [
            "| Usage | Description |",
            "| --- | --- |",
            *[f"| {usage} | {description} |" for usage, description in rows],
        ]
        body = (
            f"# {name}\n\n{summary}\n\n"
            "## Usage\n\n" + "\n".join(table) + "\n"
        )
        derived.append(
            derived_page(
                page_id=f"command.{name}",
                title=f"{name} command",
                section="command",
                summary=summary,
                alias=name,
                keywords=f"{name} command shell syntax usage examples",
                packages_any=condition_packages(condition, known_packages),
                markdown=body,
                contract="\n".join(table),
                source_path=path,
                source_href=COMMAND_GITHUB_HREFS.get(name, "commands.md"),
                condition=condition,
            )
        )
    missing = sorted(set(conditions) - set(commands))
    if missing:
        raise ValueError(f"commands.md omits registered commands: {', '.join(missing)}")
    return derived


def markdown_to_terminal_text(markdown: str) -> str:
    output: list[str] = []
    paragraph: list[str] = []
    fenced = False

    def flush_paragraph() -> None:
        if not paragraph:
            return
        output.append(" ".join(part.strip() for part in paragraph).strip())
        paragraph.clear()

    for raw in markdown.splitlines():
        line = raw.rstrip()
        if line.startswith("```"):
            flush_paragraph()
            fenced = not fenced
            if not fenced:
                output.append("")
            continue
        if fenced:
            output.append("  " + line)
            continue

        heading = heading_text(line)
        if heading is not None:
            flush_paragraph()
            if output and output[-1] != "":
                output.append("")
            output.append(strip_inline_markdown(heading[1]).upper())
            output.append("")
            continue

        stripped = line.strip()
        if not stripped:
            flush_paragraph()
            if output and output[-1] != "":
                output.append("")
            continue
        if stripped == "---":
            flush_paragraph()
            output.extend(("--------------------------------", ""))
            continue
        if re.match(r"^[-*+]\s+", stripped):
            flush_paragraph()
            output.append("- " + strip_inline_markdown(stripped[2:]))
            continue
        numbered = re.match(r"^(\d+)[.)]\s+(.+)$", stripped)
        if numbered is not None:
            flush_paragraph()
            output.append(
                f"{numbered.group(1)}. {strip_inline_markdown(numbered.group(2))}"
            )
            continue
        if stripped.startswith(">"):
            flush_paragraph()
            output.append("  " + strip_inline_markdown(stripped[1:].lstrip()))
            continue
        paragraph.append(strip_inline_markdown(stripped))

    flush_paragraph()
    while output and output[-1] == "":
        output.pop()
    return "\n".join(output) + "\n"


def split_agent_reference(text: str) -> list[tuple[int, int]]:
    """Return character ranges that remain small after UTF-8 encoding."""
    chunks: list[tuple[int, int]] = []
    start = 0
    while start < len(text):
        used = 0
        limit = start
        while limit < len(text):
            encoded = len(text[limit].encode("utf-8"))
            if used + encoded > AGENT_REFERENCE_CHUNK_MAX:
                break
            used += encoded
            limit += 1
        if limit == len(text):
            end = limit
        else:
            minimum = start + max(1, (limit - start) // 3)
            end = text.rfind("\n\n", minimum, limit)
            if end >= minimum:
                end += 2
            else:
                end = text.rfind("\n", minimum, limit)
                if end >= minimum:
                    end += 1
                else:
                    end = text.rfind(" ", minimum, limit)
                    if end >= minimum:
                        end += 1
                    else:
                        end = limit
        chunks.append((start, end))
        start = end
    return chunks


def derive_agent_references(
    page_id: str,
    markdown: str,
    body: str,
    packages: list[str],
    path: Path,
) -> list[dict[str, object]]:
    """Index focused manual excerpts without duplicating their body strings."""
    lines = markdown.splitlines()
    boundaries = [
        index
        for index, line in enumerate(lines)
        if (heading := heading_text(line)) is not None and heading[0] in (2, 3)
    ]
    sections: list[tuple[int, int, str]] = []
    first = boundaries[0] if boundaries else len(lines)
    if first > 0:
        sections.append((0, first, "Overview"))
    for position, start in enumerate(boundaries):
        end = boundaries[position + 1] if position + 1 < len(boundaries) else len(lines)
        heading = heading_text(lines[start])
        assert heading is not None
        sections.append((start, end, strip_inline_markdown(heading[1])))

    references: list[dict[str, object]] = []
    search_from = 0
    for start, end, section in sections:
        if section.casefold() == QUICK_REFERENCE_HEADING:
            continue
        rendered = markdown_to_terminal_text("\n".join(lines[start:end]))
        body_offset = body.find(rendered, search_from)
        if body_offset < 0:
            raise ValueError(
                f"{path}: agent reference section {section!r} is not in rendered body"
            )
        search_from = body_offset + len(rendered)
        ranges = split_agent_reference(rendered)
        topic = f"{page_id}.{topic_slug(section)}"
        for part, (chunk_start, chunk_end) in enumerate(ranges, start=1):
            absolute = body_offset + chunk_start
            excerpt = rendered[chunk_start:chunk_end]
            references.append(
                {
                    "page_id": page_id,
                    "topic": topic,
                    "section": section,
                    "offset": len(body[:absolute].encode("utf-8")),
                    "length": len(excerpt.encode("utf-8")),
                    "part": part,
                    "parts": len(ranges),
                    "packages_any": packages,
                }
            )
    return references


def load_pages(source: Path, packages_path: Path) -> list[dict[str, object]]:
    if not source.is_dir():
        raise ValueError("manual input must be a directory of Markdown pages")
    package_document = tomllib.loads(packages_path.read_text(encoding="utf-8"))
    known_packages = set(package_document.get("packages", {}))
    repository = source.resolve().parents[1]
    command_conditions = registry_conditions(
        repository / "src/apps/solar_os_shell.c",
        "shell_builtin_commands[]",
        r'\{"([a-z0-9-]+)"\s*,',
    )
    application_conditions = registry_conditions(
        repository / "src/apps/solar_os_app_registry.c",
        "registered_apps[]",
        r'APP_(?:FILE_)?ENTRY\("([a-z0-9-]+)"\s*,',
    )

    pages: list[dict[str, object]] = []
    seen_ids: set[str] = set()
    for path in sorted(source.glob("*.md")):
        if path.name.casefold() == "readme.md":
            continue
        metadata, markdown = parse_front_matter(path)
        page = dict(metadata)
        for field in ("id", "title", "section", "summary", "keywords"):
            value = page.get(field)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{path}: invalid {field}")
            page[field] = value.strip()

        page_id = str(page["id"])
        if not ID_RE.fullmatch(page_id):
            raise ValueError(f"invalid manual page id: {page_id}")
        if path.stem != page_id:
            raise ValueError(
                f"{path}: filename must match manual page id {page_id}"
            )
        if page_id in seen_ids:
            raise ValueError(f"duplicate manual page id: {page_id}")
        seen_ids.add(page_id)

        aliases = page.get("aliases", [])
        if not isinstance(aliases, list) or not all(
            isinstance(alias, str) and ID_RE.fullmatch(alias.strip())
            for alias in aliases
        ):
            raise ValueError(f"{page_id}: aliases must be valid topic names")
        page["aliases"] = [alias.strip() for alias in aliases]

        section = str(page["section"])
        if section not in SECTION_INFO:
            raise ValueError(f"{page_id}: unknown manual section {section}")
        page["section_title"] = SECTION_INFO[section][1]

        packages_any = page.get("packages_any", [])
        if not isinstance(packages_any, list) or not all(
            isinstance(package, str) and package for package in packages_any
        ):
            raise ValueError(f"{page_id}: packages_any must contain package IDs")
        unknown = sorted(set(packages_any) - known_packages)
        if unknown:
            raise ValueError(f"{page_id}: unknown packages: {', '.join(unknown)}")
        page["packages_any"] = packages_any
        page["path"] = path
        page["github_href"] = path.name
        page["markdown"] = markdown
        page["body"] = markdown_to_terminal_text(markdown)
        page["contract"] = extract_quick_reference(markdown, path)
        agent_sections = page.get("agent_reference_sections", False)
        if not isinstance(agent_sections, bool):
            raise ValueError(f"{path}: agent_reference_sections must be a boolean")
        page["agent_reference_sections"] = agent_sections
        page["agent_references"] = (
            derive_agent_references(
                page_id,
                markdown,
                str(page["body"]),
                packages_any,
                path,
            )
            if agent_sections
            else []
        )
        page["release_markdown"] = path.read_text(encoding="utf-8")
        page["derived"] = False
        pages.append(page)

    if not pages:
        raise ValueError("manual source must contain at least one Markdown page")

    pages.extend(
        derive_command_pages(
            source,
            pages,
            known_packages,
            command_conditions,
        )
    )
    pages.extend(
        derive_application_pages(
            source,
            pages,
            known_packages,
            application_conditions,
        )
    )
    pages.extend(derive_job_pages(source, pages, known_packages))

    seen_names: dict[str, str] = {}
    for page in pages:
        page_id = str(page["id"])
        names = [page_id, *page["aliases"]]
        for index, name in enumerate(names):
            folded = str(name).casefold()
            owner = seen_names.get(folded)
            if owner is not None:
                if index > 0 and bool(page["derived"]):
                    page["aliases"] = [
                        alias
                        for alias in page["aliases"]
                        if str(alias).casefold() != folded
                    ]
                    continue
                raise ValueError(
                    f"manual topic name {name} is shared by {owner} and {page_id}"
                )
            seen_names[folded] = page_id
        if bool(page["derived"]):
            page["release_markdown"] = release_markdown(page)
    return sorted(
        pages,
        key=lambda page: (
            SECTION_INFO[str(page["section"])][0],
            str(page["title"]).casefold(),
            str(page["id"]),
        ),
    )


def render_header(pages: list[dict[str, object]], source: Path) -> str:
    lines = [
        "/* Generated by scripts/generate_manual.py. Do not edit. */",
        f"/* Source: {source.name}/*.md */",
        "#pragma once",
        "",
        "static const solar_os_manual_page_t SOLAR_OS_MANUAL_GENERATED_PAGES[] = {",
    ]
    for page in pages:
        packages = list(page["packages_any"])
        condition = str(page.get("condition", ""))
        if not condition and packages:
            condition = " || ".join(package_macro(package) for package in packages)
        if condition:
            lines.append("#if " + condition)
        aliases = "\n".join(page["aliases"])
        lines.extend(
            [
                "    {",
                f"        .id = {c_string(str(page['id']))},",
                f"        .title = {c_string(str(page['title']))},",
                f"        .section = {c_string(str(page['section']))},",
                f"        .section_title = {c_string(str(page['section_title']))},",
                f"        .summary = {c_string(str(page['summary']))},",
                f"        .aliases = {c_string(aliases)},",
                f"        .keywords = {c_string(str(page['keywords']))},",
                f"        .body = {c_string(str(page['body']))},",
                f"        .contract = {c_string(str(page['contract']))},",
                "#if SOLAR_OS_PACKAGE_APP_READER",
                f"        .markdown = {c_string(str(page['markdown']))},",
                "#else",
                "        .markdown = NULL,",
                "#endif",
                "    },",
            ]
        )
        if condition:
            lines.append("#endif")
    lines.extend(
        [
            "};",
            "",
            "#define SOLAR_OS_MANUAL_GENERATED_PAGE_COUNT \\",
            "    (sizeof(SOLAR_OS_MANUAL_GENERATED_PAGES) / \\",
            "     sizeof(SOLAR_OS_MANUAL_GENERATED_PAGES[0]))",
            "",
            "static const solar_os_manual_reference_t "
            "SOLAR_OS_MANUAL_GENERATED_REFERENCES[] = {",
            "    {0},",
        ]
    )
    for page in pages:
        for reference in page.get("agent_references", []):
            packages = list(reference["packages_any"])
            if packages:
                lines.append(
                    "#if " + " || ".join(package_macro(package) for package in packages)
                )
            lines.extend(
                [
                    "    {",
                    f"        .page_id = {c_string(str(reference['page_id']))},",
                    f"        .topic = {c_string(str(reference['topic']))},",
                    f"        .section = {c_string(str(reference['section']))},",
                    f"        .offset = {int(reference['offset'])}U,",
                    f"        .length = {int(reference['length'])}U,",
                    f"        .part = {int(reference['part'])}U,",
                    f"        .parts = {int(reference['parts'])}U,",
                    "    },",
                ]
            )
            if packages:
                lines.append("#endif")
    lines.extend(
        [
            "};",
            "",
            "#define SOLAR_OS_MANUAL_GENERATED_REFERENCE_COUNT \\",
            "    (sizeof(SOLAR_OS_MANUAL_GENERATED_REFERENCES) / \\",
            "     sizeof(SOLAR_OS_MANUAL_GENERATED_REFERENCES[0]) - 1U)",
            "",
        ]
    )
    return "\n".join(lines)


def build_archive(pages: list[dict[str, object]]) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(
        output,
        mode="w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as archive:
        for page in pages:
            info = zipfile.ZipInfo(
                filename=f"manual/{page['id']}.md",
                date_time=(1980, 1, 1, 0, 0, 0),
            )
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(
                info,
                str(page["release_markdown"]).encode("utf-8"),
                compress_type=zipfile.ZIP_DEFLATED,
                compresslevel=9,
            )
    return output.getvalue()


def validate_release_pages(pages: list[dict[str, object]]) -> None:
    for page in pages:
        page_id = str(page["id"])
        size = len(str(page["release_markdown"]).encode("utf-8"))
        if size == 0 or size > RELEASE_PAGE_MAX:
            raise ValueError(
                f"manual page {page_id!r} is {size} bytes; "
                f"release pages must be 1..{RELEASE_PAGE_MAX} bytes"
            )


def render_catalog(
    pages: list[dict[str, object]], version: str, archive: bytes
) -> str:
    catalog_pages: list[dict[str, object]] = []
    revision_hash = hashlib.sha256()
    archive_digest = hashlib.sha256(archive).hexdigest()
    revision_hash.update(
        f"{CATALOG_SCHEMA_VERSION}:{ARCHIVE_PATH}:{archive_digest}\n".encode(
            "ascii"
        )
    )
    for page in pages:
        markdown = str(page["release_markdown"]).encode("utf-8")
        digest = hashlib.sha256(markdown).hexdigest()
        relative = f"manual/{page['id']}.md"
        revision_hash.update(relative.encode("utf-8"))
        revision_hash.update(b"\0")
        revision_hash.update(digest.encode("ascii"))
        revision_hash.update(b"\n")
        catalog_pages.append(
            {
                "id": page["id"],
                "title": page["title"],
                "section": page["section"],
                "section_title": page["section_title"],
                "summary": page["summary"],
                "aliases": page["aliases"],
                "keywords": page["keywords"],
                "packages_any": page["packages_any"],
                "path": relative,
                "size": len(markdown),
                "sha256": digest,
                "reference": page["contract"],
            }
        )
    catalog = {
        "schema": "solaros.manual_catalog",
        "schema_version": CATALOG_SCHEMA_VERSION,
        "firmware_version": version,
        "revision": revision_hash.hexdigest()[:16],
        "archive": {
            "path": ARCHIVE_PATH,
            "size": len(archive),
            "sha256": archive_digest,
        },
        "pages": catalog_pages,
    }
    # This signed wire artifact is downloaded into a bounded device buffer.
    # Keep it compact so formatting does not consume firmware-side headroom.
    return json.dumps(
        catalog, separators=(",", ":"), ensure_ascii=True
    ) + "\n"


def render_github_index(pages: list[dict[str, object]]) -> str:
    lines = [
        "# SolarOS User Manual",
        "",
        "This is the canonical documentation used by GitHub, the generated "
        "solar-os.eu website, the signed on-device `help` browser, `man`, "
        "and the native agent reference tool.",
        "",
    ]
    current_section = None
    for page in pages:
        section = str(page["section"])
        if section != current_section:
            if current_section is not None:
                lines.append("")
            lines.extend((f"## {page['section_title']}", ""))
            current_section = section
        lines.append(
            f"- [{page['title']}]({page['github_href']}) — {page['summary']}"
        )
    lines.extend(
        (
            "",
            "The TOML frontmatter on each topic controls package availability, "
            "search metadata, and placement in the documentation tree. Edit "
            "the topic itself; do not maintain a separate device or website copy.",
            "",
        )
    )
    return "\n".join(lines)


def write_release_pages(
    pages: list[dict[str, object]], output_dir: Path
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    expected: set[Path] = set()
    for page in pages:
        path = output_dir / f"{page['id']}.md"
        expected.add(path)
        content = str(page["release_markdown"])
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            path.write_text(content, encoding="utf-8")
    for path in output_dir.glob("*.md"):
        if path not in expected:
            path.unlink()


def write_archive(archive: bytes, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != archive:
        output.write_bytes(archive)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--packages", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--catalog-output", type=Path)
    parser.add_argument("--github-index-output", type=Path)
    parser.add_argument("--release-output-dir", type=Path)
    parser.add_argument("--archive-output", type=Path)
    parser.add_argument("--version")
    args = parser.parse_args()
    if (
        args.output is None
        and args.catalog_output is None
        and args.github_index_output is None
        and args.release_output_dir is None
        and args.archive_output is None
    ):
        parser.error(
            "at least one of --output, --catalog-output, or "
            "--github-index-output, --release-output-dir, or "
            "--archive-output is required"
        )
    if args.catalog_output is not None and not args.version:
        parser.error("--version is required with --catalog-output")
    if args.catalog_output is not None and args.archive_output is None:
        parser.error("--archive-output is required with --catalog-output")

    pages = load_pages(args.input, args.packages)
    if (
        args.catalog_output is not None
        or args.release_output_dir is not None
        or args.archive_output is not None
    ):
        validate_release_pages(pages)
    archive = (
        build_archive(pages)
        if args.archive_output is not None or args.catalog_output is not None
        else None
    )
    if args.output is not None:
        output = render_header(pages, args.input)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.exists() or args.output.read_text() != output:
            args.output.write_text(output)
    if args.catalog_output is not None:
        assert archive is not None
        catalog = render_catalog(pages, args.version, archive)
        args.catalog_output.parent.mkdir(parents=True, exist_ok=True)
        if (
            not args.catalog_output.exists()
            or args.catalog_output.read_text() != catalog
        ):
            args.catalog_output.write_text(catalog)
    if args.github_index_output is not None:
        index = render_github_index(pages)
        args.github_index_output.parent.mkdir(parents=True, exist_ok=True)
        if (
            not args.github_index_output.exists()
            or args.github_index_output.read_text() != index
        ):
            args.github_index_output.write_text(index)
    if args.release_output_dir is not None:
        write_release_pages(pages, args.release_output_dir)
    if args.archive_output is not None:
        assert archive is not None
        write_archive(archive, args.archive_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
