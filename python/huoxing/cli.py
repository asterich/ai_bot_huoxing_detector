import argparse
import json
import uuid
from pathlib import Path

from . import Detector, MAX_BYTES, result_dict


def main():
    parser = argparse.ArgumentParser(description="登记图片并查询此前是否出现")
    parser.add_argument("image", type=Path)
    parser.add_argument("--db", default="data/huoxing.sqlite")
    parser.add_argument("--scope", default="qq:demo:demo")
    parser.add_argument("--message-id")
    args = parser.parse_args()
    try:
        with args.image.open("rb") as stream:
            data = stream.read(MAX_BYTES + 1)
        result = Detector(args.db).ingest(data, scope=args.scope,
                                         message_id=args.message_id or uuid.uuid4().hex)
    except (OSError, ValueError, RuntimeError) as exc:
        parser.exit(2, f"处理失败：{exc}\n")
    print(json.dumps(result_dict(result), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
