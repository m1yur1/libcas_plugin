IF NOT EXIST res mkdir res
fxc /nologo /T cs_5_0 /Qstrip_reflect /Fo res\cas.cso src\cas.hlsl
fxc /nologo /T cs_5_0 /Qstrip_reflect /D USE_FP16=1 /Fo res\cashalf.cso src\cas.hlsl
