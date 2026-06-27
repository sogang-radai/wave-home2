import os
import subprocess
import shutil
from pathlib import Path
import torch

from networks.PointNetV1nCNNV1 import BedNet, SEQUENCE_LENGTH

MODEL_NAME = "bed_net0-0"
ENCODER_MODEL_NAME = "pointnet"
STATUS_MODEL_NAME = "cnn_status"
TOSS_MODEL_NAME = "cnn_toss"

INPUT_MODEL_DIR = f"./trainning/{MODEL_NAME}/"
OUTPUT_MODEL_DIR = f"../models/{MODEL_NAME}/"
PNNX_EXECUTABLE_PATH = "../thirdparty/pnnx/pnnx.exe"

def get_unique_base_name(directory: Path, name: str):
    target_param = directory / f"{name}.param"
    if not target_param.exists():
        return name

    counter = 1
    while (directory / f"{name}{counter}.param").exists():
        counter += 1

    return f"{name}{counter}"

def main():
    base_dir = Path(__file__).parent.absolute()
    pt_dir = base_dir / "pt_models"
    models_dir = Path(OUTPUT_MODEL_DIR)
    
    pnnx_exe = Path(PNNX_EXECUTABLE_PATH)
    model_src = Path(INPUT_MODEL_DIR) / "best_model.pth"

    pt_dir.mkdir(parents=True, exist_ok=True)
    models_dir.mkdir(parents=True, exist_ok=True)

    if not model_src.exists():
        print(f"Error: Cannot find model file: {model_src}")
        return
    if not pnnx_exe.exists():
        print(f"Error: Cannot find pnnx tool: {pnnx_exe}")
        return

    print(f"Loading PyTorch model: {model_src}")
    device = torch.device("cpu")

    model = BedNet()
    model.load_state_dict(torch.load(model_src, map_location=device, weights_only=True))
    model.eval()

    sub_models = [
        {
            "name": ENCODER_MODEL_NAME,
            "module": model.frame_encoder,
            "dummy": torch.randn(1, 5, 200),
            "input_shape": "[1,5,200]"
        },
        {
            "name": STATUS_MODEL_NAME,
            "module": model.head_status,
            "dummy": torch.randn(1, SEQUENCE_LENGTH, 256),
            "input_shape": f"[1,{SEQUENCE_LENGTH},256]"
        },
        {
            "name": TOSS_MODEL_NAME,
            "module": model.head_toss,
            "dummy": torch.randn(1, SEQUENCE_LENGTH, 256),
            "input_shape": f"[1,{SEQUENCE_LENGTH},256]"
        }
    ]

    for item in sub_models:
        name = item["name"]
        pt_path = pt_dir / f"{name}.pt"
        
        print(f"Tracing {name} to TorchScript...")
        traced_module = torch.jit.trace(item["module"], item["dummy"])
        traced_module.save(pt_path)

        print(f"Converting {name}.pt to ncnn via pnnx...")
        try:
            subprocess.run([
                str(pnnx_exe),
                str(pt_path),
                f"inputshape={item['input_shape']}"
            ], check=True, cwd=str(pt_dir))
            
            pnnx_param = pt_dir / f"{name}.ncnn.param"
            pnnx_bin = pt_dir / f"{name}.ncnn.bin"
            
            if not pnnx_param.exists():
                print(f"Error: Cannot find pnnx conversion output: {pnnx_param}")
                continue

            unique_name = get_unique_base_name(models_dir, name)
            final_param = models_dir / f"{unique_name}.param"
            final_bin = models_dir / f"{unique_name}.bin"

            shutil.move(str(pnnx_param), str(final_param))
            shutil.move(str(pnnx_bin), str(final_bin))
            
            print(f"Success: Saved to {final_param} and {final_bin}")

        except subprocess.CalledProcessError as e:
            print(f"pnnx conversion failed for {name}: {e}")
        
        for extra in pt_dir.glob(f"{name}*"):
            if extra.name not in [final_param.name, final_bin.name]:
                try: os.remove(extra)
                except: pass

    try: pt_dir.rmdir()
    except: pass
    
    print("\nAll conversion tasks finished.")

if __name__ == "__main__":
    main()