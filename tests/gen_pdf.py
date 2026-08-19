# -*- coding: utf-8 -*-
"""生成 PDF 产品文档：使用说明书.md / 技术报告.md → PDF（Word COM 渲染 HTML）。

用法：
  python tests/gen_pdf.py                     # 两份都生成到项目根
  python tests/gen_pdf.py <out.pdf> [<in.md>] # 单份，自定义路径

依赖：Windows + Microsoft Word（COM），PDF 被占用时自动结束 WINWORD。
"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def md_to_html(md_text):
    """极简 md → HTML：标题/表格/列表/引用/粗体/行内代码/分隔线。"""
    lines = md_text.replace("\r\n", "\n").split("\n")
    out = ["<!DOCTYPE html><html><head><meta charset='utf-8'>",
           "<style>body{font-family:'Microsoft YaHei',sans-serif;font-size:11pt;"
           "line-height:1.6;}table{border-collapse:collapse;margin:8px 0;}td,th{"
           "border:1px solid #999;padding:4px 8px;}code{font-family:Consolas;"
           "background:#f2f2f2;}blockquote{color:#555;border-left:3px solid #aaa;"
           "margin:6px 0;padding:2px 12px;}</style></head><body>"]
    in_list = False
    in_table = False
    for raw in lines:
        line = raw.rstrip()
        if not line.strip():
            if in_list:
                out.append("</ul>")
                in_list = False
            if in_table:
                out.append("</table>")
                in_table = False
            continue
        if line.startswith("### "):
            if in_list:
                out.append("</ul>")
                in_list = False
            out.append("<h3>%s</h3>" % _fmt(line[4:]))
        elif line.startswith("## "):
            if in_list:
                out.append("</ul>")
                in_list = False
            out.append("<h2>%s</h2>" % _fmt(line[3:]))
        elif line.startswith("# "):
            if in_list:
                out.append("</ul>")
                in_list = False
            out.append("<h1>%s</h1>" % _fmt(line[2:]))
        elif line.startswith("---"):
            if in_list:
                out.append("</ul>")
                in_list = False
            if in_table:
                out.append("</table>")
                in_table = False
            out.append("<hr>")
        elif line.startswith("|"):
            cells = [c.strip() for c in line.strip("|").split("|")]
            if not in_table:
                out.append("<table>")
                in_table = True
                out.append("<tr><th>%s</th></tr>" % "</th><th>".join(_fmt(c) for c in cells))
            else:
                if all(re.fullmatch(r":?-{2,}:?", c) for c in cells):
                    continue  # 分隔行
                out.append("<tr><td>%s</td></tr>" % "</td><td>".join(_fmt(c) for c in cells))
        elif line.startswith("- "):
            if not in_list:
                out.append("<ul>")
                in_list = True
            out.append("<li>%s</li>" % _fmt(line[2:]))
        elif line.startswith("> "):
            if in_list:
                out.append("</ul>")
                in_list = False
            out.append("<blockquote>%s</blockquote>" % _fmt(line[2:]))
        else:
            if in_list:
                out.append("</ul>")
                in_list = False
            if in_table:
                out.append("</table>")
                in_table = False
            out.append("<p>%s</p>" % _fmt(line))
    if in_list:
        out.append("</ul>")
    if in_table:
        out.append("</table>")
    out.append("</body></html>")
    return "\n".join(out)


def _fmt(text):
    t = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    t = re.sub(r"`(.+?)`", r"<code>\1</code>", t)
    return t


def word_html_to_pdf(html_path, pdf_path):
    """Word COM：打开 HTML → 导出 PDF。PDF 被占用先结束 WINWORD。"""
    ps = (
        "$ErrorActionPreference='Stop'\n"
        "Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force\n"
        "$w = New-Object -ComObject Word.Application\n"
        "$w.Visible = $false\n"
        "$d = $w.Documents.Open('%s')\n"
        "$d.ExportAsFixedFormat('%s', 17)\n"
        "$d.Close(0)\n"
        "$w.Quit()\n"
        "[System.Runtime.Interopservices.Marshal]::ReleaseComObject($w) | Out-Null\n"
        "Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force\n"
    ) % (html_path.replace("'", "''"), pdf_path.replace("'", "''"))
    p = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                       capture_output=True, timeout=180)
    if p.returncode != 0:
        raise RuntimeError("Word COM 失败: %s" % p.stderr.decode("utf-8", "replace")[:500])
    if not os.path.exists(pdf_path):
        raise RuntimeError("PDF 未生成")


def convert(md_path, pdf_path):
    with open(md_path, "r", encoding="utf-8") as f:
        html = md_to_html(f.read())
    html_path = os.path.join(os.environ.get("TEMP", ROOT), "creeper_doc_tmp.html")
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html)
    try:
        word_html_to_pdf(html_path, pdf_path)
        print("written %s" % pdf_path)
    finally:
        if os.path.exists(html_path):
            os.remove(html_path)


def main():
    pairs = []
    if len(sys.argv) >= 3:
        pairs.append((sys.argv[2], sys.argv[1]))
    else:
        pairs = [
            (os.path.join(ROOT, "docs", "使用说明书.md"), os.path.join(ROOT, "res", "使用说明书.pdf")),
            (os.path.join(ROOT, "docs", "技术报告.md"), os.path.join(ROOT, "res", "技术报告.pdf")),
        ]
    for md_path, pdf_path in pairs:
        convert(md_path, pdf_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())