# so, to import a model to an esp, you need to basically import it as a a C array. what we're doing is taking the binary file and making it an array now like this

from pathlib import Path
model = Path("alt_cnn_model_uint8.tflite").read_bytes()

ouputFile = Path("EspIdfCode/main/model_data.cpp")

with open(ouputFile, "w") as f:
    # begin writing the array
    f.write("extern \"C\" const unsigned char modelWeights[] = {\n")
    # go through all the bytes in the modle
    for i, b in enumerate(model):
        # when actually writing the weight, we want to store it as hex format and store 2 digits, like 0xa5, etc.
        f.write(f"0x{b:02x}, ")
        # do a new line every so often so its readable 
        if (i + 1) % 12 == 0: 
            f.write("\n")
    f.write("\n};\n")
    # actually have to save the length separately cause .size doesn't work on C arrays allegedly?
    f.write(f"extern \"C\" const unsigned int modelLen = {len(model)};\n")

print(f"Conversion done!")

