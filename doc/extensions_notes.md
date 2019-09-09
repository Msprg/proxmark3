# Notes on file extensions

The Proxmark3 client uses a wide range of files. Here is a brief recap to get you up to speed.

| extension | description|
|---|---|
| .exe | windows executable |
| .bin | binary file,  can be firmware or memory dump of a tag |
| .eml | text file, with memory dump of a tag |
| .mfd | binary file,  usually created with Mifare Classic Tool app (MCT),  contains memory dump of tag. Very similar to .bin file |
| .json | JSON file, usually settings file or it can also be a memory dump of a tag |
| .dic | dictionary file. textual, with keys/passwords one line / key |
| .elf | binary proxmark3 device firmware file. |
| .cmd | text file, contains proxmark3 client commands used to call client with -s |
| .lua | text file, contains lua script to be run inside client.  or called with -l |
| .pm3 | text file, with numbers ranging 0-255 or -127 - 128.  Contains trace signal data for low frequency tags (data load) |
| .trace | binary file,  contains trace log data usually from high frequency tags.  (hw trace load) |