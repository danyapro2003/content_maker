"""Подмешивает DLL из pip-пакетов nvidia (cudnn/cublas) в PATH до импорта onnxruntime.

На Windows onnxruntime-gpu ищет cudnn64_9.dll через PATH/LoadLibrary, а pip-пакет
кладёт их в site-packages/nvidia/*/bin — туда PATH не указывает. Импортируй этот
модуль ПЕРВЫМ, до onnxruntime/ctranslate2.
"""
import glob
import os
import site
import sys


def _add_nvidia_dlls():
    roots = []
    try:
        roots.extend(site.getsitepackages())
    except Exception:
        pass
    if site.getusersitepackages():
        roots.append(site.getusersitepackages())
    # на всякий случай — рядом с интерпретатором
    roots.append(os.path.dirname(os.path.dirname(sys.executable)))

    added = []
    for root in roots:
        nvidia = os.path.join(root, "nvidia")
        if not os.path.isdir(nvidia):
            continue
        for d in glob.glob(os.path.join(nvidia, "*", "bin")):
            if d not in added:
                added.append(d)
    if added:
        os.environ["PATH"] = os.pathsep.join(added + [os.environ.get("PATH", "")])
        # python 3.8+ не использует PATH для DLL из расширений — добавляем явно
        for d in added:
            try:
                os.add_dll_directory(d)
            except Exception:
                pass
    return added


ADDED_DIRS = _add_nvidia_dlls()

if __name__ == "__main__":
    print("\n".join(ADDED_DIRS) or "ничего не найдено")
