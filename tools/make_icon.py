"""
TauriCPP 多分辨率图标生成工具
从源 PNG 图像生成包含多种分辨率的 .ico 文件

标准 Windows 图标分辨率: 16, 24, 32, 48, 64, 128, 256

依赖: pip install Pillow
"""
import os
import sys
import argparse
from PIL import Image

DEFAULT_SIZES = [16, 24, 32, 48, 64, 128, 256]


def make_icon(source_path, output_path, sizes=None):
    if sizes is None:
        sizes = DEFAULT_SIZES

    if not os.path.isfile(source_path):
        print(f"Error: source image not found: {source_path}")
        sys.exit(1)

    img = Image.open(source_path)
    if img.mode != "RGBA":
        img = img.convert("RGBA")

    frames = []
    for size in sizes:
        resized = img.resize((size, size), Image.LANCZOS)
        frames.append(resized)

    frames[-1].save(
        output_path,
        format="ICO",
        sizes=[(f.width, f.height) for f in frames],
        append_images=frames[:-1],
    )

    size_kb = os.path.getsize(output_path) / 1024
    print(f"Generated {output_path} ({size_kb:.1f} KB)")
    print(f"  Sizes: {', '.join(str(s) for s in sizes)}")


def main():
    parser = argparse.ArgumentParser(description="TauriCPP Multi-Resolution Icon Generator")
    parser.add_argument("source", help="Source PNG image path")
    parser.add_argument("-o", "--output", default="icon.ico", help="Output .ico file path")
    parser.add_argument(
        "-s", "--sizes",
        default=None,
        help="Comma-separated sizes (default: 16,24,32,48,64,128,256)",
    )
    args = parser.parse_args()

    sizes = None
    if args.sizes:
        try:
            sizes = [int(s.strip()) for s in args.sizes.split(",")]
        except ValueError:
            print("Error: invalid size list, must be comma-separated integers")
            sys.exit(1)

    make_icon(args.source, args.output, sizes)


if __name__ == "__main__":
    main()
