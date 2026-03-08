import os

def tflite_to_c_array(tflite_path, output_path=None):
    base_name = os.path.splitext(os.path.basename(tflite_path))[0]
    var_name = "g_" + base_name.replace("-", "_").replace(" ", "_")

    if output_path is None:
        output_path = base_name + "_data.cpp"

    with open(tflite_path, "rb") as f:
        data = f.read()

    # Wrap at 12 bytes per line for readability
    bytes_list = [f"0x{b:02x}" for b in data]
    lines = []
    for i in range(0, len(bytes_list), 12):
        lines.append("  " + ", ".join(bytes_list[i:i+12]))
    hex_block = ",\n".join(lines)

    header_name = os.path.splitext(os.path.basename(output_path))[0] + ".h"
    header_path = os.path.join(os.path.dirname(output_path), header_name)

    cc_content = f"""\
#include "{header_name}"

// Auto-generated from {os.path.basename(tflite_path)}
// Model size: {len(data)} bytes

alignas(8) const uint8_t {var_name}[] = {{
{hex_block}
}};

const int {var_name}_len = {len(data)};
"""

    h_content = f"""\
#pragma once
#include <cstdint>

// Auto-generated from {os.path.basename(tflite_path)}
extern const uint8_t {var_name}[];
extern const int {var_name}_len;
"""

    with open(output_path, "w") as f:
        f.write(cc_content)
    with open(header_path, "w") as f:
        f.write(h_content)

    print(f"Generated: {output_path} ({len(data)} bytes)")
    print(f"Generated: {header_path}")


if __name__ == "__main__":
    tflite_to_c_array(
        tflite_path="/home/debangshup/Programming/researchProjects/MiniModelBot/hello_world/model2_quantized.tflite",
        output_path="/home/debangshup/Programming/researchProjects/MiniModelBot/hello_world/model_weights.cpp"
    )