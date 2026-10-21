"""Parse conversation prompts without dispatching tasks or changing Git."""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


def parse(text):
    lines = text.splitlines(keepends=True)
    first = next((i for i, line in enumerate(lines) if line.strip()), None)
    if first is not None and lines[first].strip() == "$prompt-scheduler":
        lines = lines[first + 1:]
    tasks, body = [], []
    current = 1
    explicit = False
    fence = None

    def append():
        prompt = "".join(body)
        if not prompt.strip():
            raise ValueError("任务 {} 为空".format(current))
        tasks.append({"index": current, "prompt": prompt,
                      "sha256": hashlib.sha256(prompt.encode("utf-8")).hexdigest()})

    for line_no, line in enumerate(lines, 1):
        stripped = line.strip()
        if fence:
            body.append(line)
            if re.fullmatch(re.escape(fence[0]) + "{" + str(fence[1]) + ",}\\s*", stripped):
                fence = None
            continue
        opening = re.match(r"^ {0,3}(`{3,}|~{3,})(.*)$", line.rstrip("\r\n"))
        if opening and not (opening[1][0] == "`" and "`" in opening[2]):
            fence = (opening[1][0], len(opening[1]))
            body.append(line)
            continue
        marker = re.fullmatch(r"【任务 ([1-9][0-9]*)】", stripped)
        if not marker:
            if re.fullmatch(r"【任务\s*[^】]*】", stripped):
                raise ValueError("第 {} 行任务标记格式错误".format(line_no))
            body.append(line)
            continue
        number = int(marker[1])
        if not explicit and not tasks and not "".join(body).strip() and number == 1:
            body = []
            explicit = True
            continue
        append()
        if number != current + 1:
            raise ValueError("期望【任务 {}】，收到【任务 {}】".format(current + 1, number))
        current = number
        explicit = True
        body = []
    if fence:
        raise ValueError("代码块未闭合，无法可靠识别任务边界")
    append()
    return {"schemaVersion": 1, "tasks": tasks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        raw = (args.input.read_bytes() if args.input else sys.stdin.buffer.read()).decode("utf-8-sig")
        result = parse(raw)
        # Exclusive creation protects saved original prompts from accidental overwrite.
        with args.output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(result, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
    except (ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print("Parsed {} task(s).".format(len(result["tasks"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
