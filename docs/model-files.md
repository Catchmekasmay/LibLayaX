# Model files: a folder, one .tar file, and codecs

A Laya model is five files in a folder, and that folder is what `laya_create` normally gets.
From 1.0.15 the library can read the model in two more ways:

* [A model in one `.tar` file](#a-model-in-one-tar-file)
* [A codec for the weights](#a-codec-for-the-weights)

Both are in every 1.0.15 library, on every platform and for the CPU and the GPU alike. Libraries
before 1.0.15 accept a folder only and do not export `laya_set_codec`
([Older libraries](#older-libraries)).

## A model in one .tar file

`laya_create` takes the path of a `.tar` file where it takes a model folder:

```c
laya_agent* agent = laya_create("C:\\models\\laya.tar", "{\"backend\":\"cpu\"}");
```

Every file of the model is then read from the archive, and nothing is unpacked to disk:

```
rl_agent_config.json
encoder/config.json
model.safetensors
tokenizer/tokenizer.json
tokenizer/tokenizer_config.json
```

Rules:

* The archive is a plain tar file: `ustar`, GNU or pax, as `tar` writes them on Windows,
  Linux and macOS. A compressed archive (`.tar.gz`, `.zip`) is refused.
* The files may sit at the top of the archive, or all inside one folder. Both of these work:

  ```
  tar -cf laya.tar -C C:\models\laya .
  tar -cf laya.tar -C C:\models laya
  ```

* The `variant` option works as with a folder: with `multilingual/…` beside the files of the
  English model, `"variant":"multilingual"` selects that folder of the archive.
* Entries named `._something`, which the macOS `tar` adds, are ignored.
* `model_dir` in the answer of `laya_info` is the path of the archive.

Loading from an archive takes the same time and memory as loading from a folder: the weights
are read from their place in the file, one tensor at a time.

| Message | Meaning |
|---|---|
| `… is not a folder and not an uncompressed tar archive` | The path names a file that is not a tar archive, or a compressed one. |
| `Cannot open …\tokenizer\tokenizer.json (no such file in the archive)` | The archive lacks that file, or the model sits deeper than one folder. |
| `Truncated tar archive: …` | The file ends before the archive does; it was cut short while being copied or written. |
| `Damaged tar archive …` | A header inside the archive is not valid. |

## A codec for the weights

A codec is a function that decodes the stored bytes of the weight file, `model.safetensors`,
while the library reads it. With one, the weight file on disk can be kept in another form than
the plain one. The codec applies to the weight file only, and in the same way whether the
model is a folder or a `.tar` file. The other files are never passed to it.

```c
typedef int (*laya_codec_fn)(void* user, const char* path_utf8, const char* file_name_utf8,
                             uint64_t offset, void* data, uint64_t size);
```

The library reads a block of the stored file and hands it to the function, which replaces it,
in place, with the same block of the plain file.

| Argument | Meaning |
|---|---|
| `user` | The pointer given to `laya_set_codec`. |
| `path_utf8` | The file on disk the block was read from, as a full path in UTF-8: the weight file itself for a model folder (`C:\models\laya\model.safetensors`), the archive for a model in a `.tar` file (`C:\models\laya.tar`). |
| `file_name_utf8` | The name of the weight file without folders: `model.safetensors`. |
| `offset` | Where `data` begins in that file, counted from the start of the file (not of an archive around it). |
| `data`, `size` | The block. The decoded block has the same size. |
| result | `0` for success. Any other number makes `laya_create` fail with `The codec reported error N for model.safetensors`. |

What follows from that:

* **The decoded file has the same length as the stored one**, byte for byte in the same place.
  A transform that changes the length (compression, for example) does not fit this interface.
* **Blocks come in any order and any size.** The function must be able to decode a block from
  its offset alone. The library asks for the first 8 bytes, then the header, then one block per
  tensor: about 200 calls for the English model.
* **A codec can keep a file of its own beside the model.** `path_utf8` tells it where the
  model is: the folder of that path is the model folder, or the folder the `.tar` file is in.
  The library does not read or look for such a file; opening it is up to the codec. The path is
  the same in every call of one load, so it is enough to do that in the first call.
* The function is called on the thread that calls `laya_create`, and only during that call.
  After loading, the codec is not used again.

`laya_set_codec(fn, user)` gives the library the function. It applies to every `laya_create`
that follows, on any thread; `laya_set_codec(NULL, NULL)` removes it. The setting is for the
whole process, not for one agent.

```c
static int my_decode(void* user, const char* path, const char* name, uint64_t offset,
                     void* data, uint64_t size) {
    /* turn the stored bytes at data[0..size) into the plain ones */
    return 0;
}

laya_set_codec(my_decode, NULL);
laya_agent* agent = laya_create("C:\\models\\laya.tar", "{\"backend\":\"vulkan\"}");
```

| Message | Meaning |
|---|---|
| `Invalid safetensors header size` | The weight file is in a stored form and no codec was set, or the codec is not the one the file was written for. |
| `The codec reported error N for model.safetensors` | The codec function returned `N`. |

To keep the codec in a DLL of its own, load that DLL in your program and pass the function it
exports to `laya_set_codec`.

## Older libraries

Libraries before 1.0.15 have ten functions and no `laya_set_codec`. A program that must also
run with those looks the function up at run time and does not link it:

```c
HMODULE lib = LoadLibraryA("laya.dll");
int has_codecs = GetProcAddress(lib, "laya_set_codec") != NULL;
```

The same test tells whether the library accepts a `.tar` file. The Delphi binding does the
lookup for you (`LayaCodecSupported`).

## Tested

On the build machine, on the CPU, with a test model (the real architecture with random
weights), with the Linux x86-64 library and with the Windows x64 library under Wine:

* the same answer from the model folder and from archives written as `ustar`, GNU and pax, with
  the files at the top and inside one folder, with long names, and with a variant folder;
* the same answer from a weight file in a stored form, in a folder and in an archive, through a
  test codec; and the expected message without the codec, with a codec that fails, with a file
  that is not an archive, with an archive that lacks a file and with one cut in half: 23 cases,
  all as expected on both systems;
* the library's own test suite unchanged: 54 checks, 0 failures.

Neither has been run on a GPU or on real hardware by the build; a codec set from a Delphi
program has been used on Windows with the `vulkan-compat-sse42` library.

## In the source

`laya.cpp/include/laya/model_files.hpp` and `laya.cpp/src/model_files.cpp` are the layer every
model file is read through. `laya.cpp/tests/capi/test_sources.c` is the test program
(`test-sources`); [Testing](testing.md) says how to run it.
