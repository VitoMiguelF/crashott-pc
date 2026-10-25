# stb_image

`stb_image.h` v2.30 from [nothings/stb](https://github.com/nothings/stb) (commit
`003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`, SHA-256
`594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`). It is public domain or MIT
(your choice, see the end of the file).

It is the PNG decoder of the non-Windows builds (`runtime/platform/image_posix.cpp`, PNG only).
That decoder reads the language-pack fonts and the menu art inside `default.rcf`. Windows uses WIC
(`image_win32.cpp`).
