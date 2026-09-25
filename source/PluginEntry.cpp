/**
	The bundle's own translation unit.

	There is no entry point to write. `plugMain` lives in the SDK, the host
	resolves it by name after dlopen, and everything past that follows from the
	`CFFGLPluginInfo` that Receipt.cpp registers at static-initialisation time.
	This file exists so the bundle target has a source of its own, and it
	carries the one thing worth being able to read back out of a shipped
	binary: which build it is.

		strings Receipt.bundle/Contents/MacOS/Receipt | grep receipt
*/

extern "C" const char* ReceiptBuildStamp()
{
	return "receipt " RECEIPT_VERSION " built " __DATE__ " " __TIME__;
}
