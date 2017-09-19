
solution "base"
  configurations { "Debug", "Release" }
  location "build"

  configuration { "linux", "gmake" }
    buildoptions { "-std=c99", "-fPIC", "-D_XOPEN_SOURCE=600" }

  project "base"
    kind "SharedLib"
    language "C"
    location "build"
    files { "include/*.h", "src/*.c" }
    includedirs { "." }
    if os.is64bit then
      objdir ("obj/" .. os.get() .. "-64")
    else
      objdir ("obj/" .. os.get() .. "-32")
    end

    configuration "linux"
      links { "rt", "dl", "m", "ffi" }    

    configuration "Debug"
      defines { "DEBUG" }
      flags { "Symbols" }

    configuration "Release"
      defines { "NDEBUG" }
      flags { "Optimize" }

