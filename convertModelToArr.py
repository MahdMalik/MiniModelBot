# so, to import a model to an esp, you need to basically import it as a a C array. what we're doing is taking the binary file and making it an array now like this


headless = True



from pathlib import Path
if(not headless):
    model = Path("final_model.tflite").read_bytes()
else:
    model = Path("final_model_headless.tflite").read_bytes()


if not headless:
    ouputFile = Path("main/include/model_data.h")
else:
    ouputFile = Path("main/include/headless_model.h")


with open(ouputFile, "w") as f:
    # begin writing the array
    if(not headless):
        f.write("const unsigned char modelWeights[] = {\n")
    else:
        f.write("const unsigned char modelWeightsHeadless[] = {\n")
    
    # go through all the bytes in the modle
    for i, b in enumerate(model):
        # when actually writing the weight, we want to store it as hex format and store 2 digits, like 0xa5, etc.
        f.write(f"0x{b:02x}, ")
        # do a new line every so often so its readable 
        if (i + 1) % 12 == 0: 
            f.write("\n")
    f.write("\n};\n")
    # actually have to save the length separately cause .size doesn't work on C arrays allegedly?
    if(not headless):
        f.write(f"const unsigned int modelLen = {len(model)};\n")
    else:
        f.write(f"const unsigned int modelLenHeadless = {len(model)};\n")

print(f"Conversion done!")

