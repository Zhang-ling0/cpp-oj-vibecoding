#!/usr/bin/env python3
# 生成 static/favicon.ico（32×32 与 16×16 两个尺寸，PNG-in-ICO）。
# 设计与 static/favicon.svg 一致：靛蓝→紫渐变圆角方块 + 白色 </> 代码尖括号。
# 无第三方依赖（仅标准库），8× 超采样抗锯齿。

import struct
import zlib
from pathlib import Path

SIZE = 64          # 内部绘制分辨率
SS = 8             # 每轴超采样倍数
RADIUS = 14.0      # 圆角半径
STROKE = 5.5       # 白色笔画宽度
GRAD_FROM = (0x4F, 0x46, 0xE5)  # #4f46e5
GRAD_TO = (0x7C, 0x3A, 0xED)    # #7c3aed

# 白色 </> 折线（与 favicon.svg 中路径一致）
SEGMENTS = [
    ((23.0, 19.0), (13.5, 32.0), (23.0, 45.0)),   # <
    ((41.0, 19.0), (50.5, 32.0), (41.0, 45.0)),   # >
    ((37.5, 15.0), (26.5, 49.0)),                 # /
]


def in_rounded_rect(x, y):
    """点是否在圆角方块内（抗锯齿通过超采样实现，这里做硬判定）。"""
    cx, cy = SIZE / 2.0, SIZE / 2.0
    half = SIZE / 2.0
    # 先判定大方块，再判定四个圆角外侧
    if not (0 <= x < SIZE and 0 <= y < SIZE):
        return False
    ax, ay = abs(x - cx), abs(y - cy)
    corner_x, corner_y = half - RADIUS, half - RADIUS
    if ax <= corner_x or ay <= corner_y:
        return True
    dx, dy = ax - corner_x, ay - corner_y
    return dx * dx + dy * dy <= RADIUS * RADIUS


def dist_to_segment(px, py, x1, y1, x2, y2):
    vx, vy = x2 - x1, y2 - y1
    wx, wy = px - x1, py - y1
    seg_len2 = vx * vx + vy * vy
    if seg_len2 == 0:
        t = 0.0
    else:
        t = max(0.0, min(1.0, (wx * vx + wy * vy) / seg_len2))
    dx, dy = px - (x1 + t * vx), py - (y1 + t * vy)
    return (dx * dx + dy * dy) ** 0.5


def on_stroke(x, y):
    """点是否落在白色笔画上（折线按线段并集判定，圆头由线段端点距离<=半径自然形成）。"""
    r = STROKE / 2.0
    for poly in SEGMENTS:
        for i in range(len(poly) - 1):
            (x1, y1), (x2, y2) = poly[i], poly[i + 1]
            if dist_to_segment(x, y, x1, y1, x2, y2) <= r:
                return True
    return False


def gradient_color(x, y):
    """沿对角线在两色间线性插值，返回 (r, g, b)。"""
    t = (x + y) / (2.0 * SIZE)
    t = max(0.0, min(1.0, t))
    return tuple(
        round(GRAD_FROM[i] + (GRAD_TO[i] - GRAD_FROM[i]) * t) for i in range(3)
    )


def render(out_size):
    """渲染出 out_size×out_size 的 RGBA 像素列表（每像素 4 字节）。"""
    scale = out_size / SIZE
    pixels = bytearray(out_size * out_size * 4)
    sub = SS
    for py in range(out_size):
        for px in range(out_size):
            r_acc = g_acc = b_acc = a_acc = 0.0
            for sy in range(sub):
                for sx in range(sub):
                    # 超采样点映射回 64×64 逻辑坐标
                    fx = (px + (sx + 0.5) / sub) / scale
                    fy = (py + (sy + 0.5) / sub) / scale
                    if in_rounded_rect(fx, fy):
                        if on_stroke(fx, fy):
                            r, g, b = 255, 255, 255
                        else:
                            r, g, b = gradient_color(fx, fy)
                        r_acc += r
                        g_acc += g
                        b_acc += b
                        a_acc += 255.0
            if a_acc > 0:
                # 命中采样数（每命中一针 alpha 计 255）；RGB 按命中数平均，alpha 按覆盖率
                hits = a_acc / 255.0
                idx = (py * out_size + px) * 4
                pixels[idx] = round(r_acc / hits)
                pixels[idx + 1] = round(g_acc / hits)
                pixels[idx + 2] = round(b_acc / hits)
                pixels[idx + 3] = round(a_acc / (sub * sub))
    return bytes(pixels)


def encode_png(data, size):
    """把 RGBA 像素编码为 PNG（8-bit truecolor+alpha，filter 0）。"""
    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    raw = b"".join(
        b"\x00" + data[y * size * 4:(y + 1) * size * 4] for y in range(size)
    )
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # RGBA
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def build_ico(images):
    """把若干 (size, png_bytes) 打包为 ICO 容器（PNG-in-ICO）。"""
    count = len(images)
    header = struct.pack("<HHH", 0, 1, count)
    entries = b""
    blobs = b""
    offset = 6 + 16 * count
    for size, png in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack(
            "<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), offset
        )
        blobs += png
        offset += len(png)
    return header + entries + blobs


def main():
    out = Path(__file__).resolve().parent.parent / "static" / "favicon.ico"
    images = [(s, encode_png(render(s), s)) for s in (32, 16)]
    out.write_bytes(build_ico(images))
    print(f"written: {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
