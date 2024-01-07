import argparse
import fileinput
import os
import subprocess
import sys
import threading

parser = argparse.ArgumentParser(description="Compile all GLSL shaders")
parser.add_argument("--glslang", type=str, help="path to glslangvalidator executable")
parser.add_argument("--g", action="store_true", help="compile with debug symbols")
args = parser.parse_args()


def findGlslang():
    def isExe(path):
        return os.path.isfile(path) and os.access(path, os.X_OK)

    if args.glslang != None and isExe(args.glslang):
        return args.glslang

    exe_name = "glslangvalidator"
    if os.name == "nt":
        exe_name += ".exe"

    for exe_dir in os.environ["PATH"].split(os.pathsep):
        full_path = os.path.join(exe_dir, exe_name)
        if isExe(full_path):
            return full_path

    sys.exit("Could not find DXC executable on PATH, and was not specified with --dxc")


glslang_path = findGlslang()
dir_path = os.path.dirname(os.path.realpath(__file__))
dir_path = dir_path.replace("\\", "/")


def process_file(file):
    if (
        file.endswith(".vert")
        or file.endswith(".frag")
        or file.endswith(".comp")
        or file.endswith(".geom")
        or file.endswith(".tesc")
        or file.endswith(".tese")
        or file.endswith(".rgen")
        or file.endswith(".rchit")
        or file.endswith(".rahit")
        or file.endswith(".rmiss")
    ):
        input_file = os.path.join(root, file)
        output_file = input_file + ".spv"

        add_params = ""
        if args.g:
            add_params = "-g"

        res = subprocess.call(
            "%s -V %s -o %s %s --target-env vulkan1.3 --target-env spirv1.6"
            % (glslang_path, input_file, output_file, add_params),
            shell=False,
        )
        if res != 0:
            sys.exit()


threads = []

for root, dirs, files in os.walk(dir_path):
    for file in files:
        t = threading.Thread(target=process_file, args=(file,))
        threads.append(t)
        t.start()

for t in threads:
    t.join()
