import os
import openai

# Option A: Hardcode your API key here (not recommended for production):
openai.api_key = ""

# Option B: Retrieve it from an environment variable (preferred):
# e.g., set OPENAI_API_KEY=sk-...
# openai.api_key = os.getenv("OPENAI_API_KEY", "YOUR_OPENAI_API_KEY_HERE")

def transform_exports_to_def(exports_text):
    """
    Sends the exports.txt content to ChatGPT with instructions
    to return a properly formatted .def file.
    """
    prompt = f"""
        The following is a Microsoft COFF/PE Dumper output ('exports.txt') for a file named 'webrtc.dll'.
        I want a valid webrtc.def file that:
        1) starts with the line 'LIBRARY "webrtc"'
        2) has an 'EXPORTS' line
        3) enumerates every function in the order it appears in the output
        4) must include _cgo_dummy_export if it is listed
        5) do not omit anything

        Here is the exports.txt: {exports_text}
        Now produce ONLY the .def file contents, nothing extra."""

    response = openai.ChatCompletion.create(
        model="gpt-4",
        messages=[{"role": "user", "content": prompt}],
        temperature=0,
    )

    # Extract the assistant's message text
    print(response.choices[0].message.content.strip())
    output = response.choices[0].message.content.strip()
    return output

def main():
    # 1) Load the contents of exports.txt
    with open("exports.txt", "r", encoding="utf-8") as f:
        exports_text = f.read()

    # 2) Call the function that interacts with ChatGPT
    def_file_contents = transform_exports_to_def(exports_text)

    # 3) Save the output to webrtc.def
    with open("webrtc.def", "w", encoding="utf-8") as def_file:
        def_file.write(def_file_contents + "\n")

    print("[INFO] webrtc.def created successfully!")

if __name__ == "__main__":
    main()
