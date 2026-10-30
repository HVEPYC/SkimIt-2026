// Empty lib/<abi>/libmono.so: the original player calls System.loadLibrary("mono") before
// loadLibrary("unity"). The real libmono is a guest library that libunity.so (the runner) loads.
