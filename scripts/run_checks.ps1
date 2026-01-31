# Regression checks (needs a GPU, so run locally, not in CI):
#   1. physics self-test (bh_tests)
#   2. reference renders compared against tests/golden/*.png (with a tolerance,
#      so small GPU/driver differences don't fail)
#   3. shadow size vs the analytic formula for a static observer
#
# Usage (from the repo root, after building):
#   powershell -ExecutionPolicy Bypass -File scripts/run_checks.ps1
#   ... -Exe build\blackhole_sim.exe -Tests build\bh_tests.exe   (CMake build)
#   ... -UpdateGolden     re-create the golden images after an intended change
param(
    [string]$Exe   = "main.exe",
    [string]$Tests = "bh_tests.exe",
    [switch]$UpdateGolden
)
$ErrorActionPreference = "Stop"
$root   = Split-Path -Parent $PSScriptRoot
$golden = Join-Path $root "tests\golden"
$tmp    = Join-Path ([IO.Path]::GetTempPath()) "bh_checks"
New-Item -ItemType Directory -Force $tmp, $golden | Out-Null
$Exe = (Resolve-Path $Exe).Path
$failed = 0

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging;
public static class ImgCmp {
    static byte[] Px(Bitmap b, out int stride) {
        var d = b.LockBits(new Rectangle(0,0,b.Width,b.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        stride = d.Stride; var buf = new byte[stride*b.Height];
        System.Runtime.InteropServices.Marshal.Copy(d.Scan0, buf, 0, buf.Length); b.UnlockBits(d); return buf;
    }
    // returns {mean abs difference per channel (0-255), fraction of pixels differing by > 48}
    public static double[] Compare(string a, string b) {
        using (var A = new Bitmap(a)) using (var B = new Bitmap(b)) {
            if (A.Width != B.Width || A.Height != B.Height) return new double[]{ 999, 1 };
            int sa, sb; var pa = Px(A, out sa); var pb = Px(B, out sb);
            double sum = 0; long big = 0, n = (long)A.Width*A.Height;
            for (int y = 0; y < A.Height; y++) for (int x = 0; x < A.Width; x++) {
                int d = 0;
                for (int c = 0; c < 3; c++) d += Math.Abs(pa[y*sa+x*3+c] - pb[y*sb+x*3+c]);
                sum += d; if (d > 48) big++;
            }
            return new double[]{ sum / (3.0*n), (double)big / n };
        }
    }
    // distance from the image centre to the first non-black pixel in a direction
    public static int EdgeDistance(string file, int dx, int dy) {
        using (var b = new Bitmap(file)) {
            int cx = b.Width/2, cy = b.Height/2;
            for (int k = 1; k < 400; k++) {
                var c = b.GetPixel(cx + dx*k, cy + dy*k);
                if (c.R + c.G + c.B >= 30) return k;
            }
            return -1;
        }
    }
}
"@

# 80 frames: the progressive average converges after ~40 passes, so this
# leaves margin if the window is resized (and the average restarts) early on
function Render([string]$name, [string[]]$extra, [string]$size = "640x360", [int]$frames = 80) {
    $out = Join-Path $tmp "$name.bmp"
    Remove-Item $out -ErrorAction SilentlyContinue
    & $Exe --size $size --no-ui --screenshot $out $frames @extra | Out-Null
    if (-not (Test-Path $out)) { throw "render '$name' produced no image" }
    return $out
}

# ---- 1. physics self-test ----
if (Test-Path $Tests) {
    & (Resolve-Path $Tests).Path | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] physics self-test"; $failed++ } else { Write-Host "[ ok ] physics self-test" }
} else { Write-Host "[skip] $Tests not found" }

# ---- 2. reference renders ----
$cases = [ordered]@{
    "flat-reference" = @("--spin","0","--flat-disk","--no-stars","--no-bloom","--grid","overlay")
    "video-look"     = @("--video-look")
    "kerr-flat"      = @("--flat-disk","--no-bloom")
    "kerr-full"      = @("--time","2")
    "thick-disk"     = @("--time","2","--thick-disk")
}
foreach ($name in $cases.Keys) {
    $img = Render $name $cases[$name]
    $ref = Join-Path $golden "$name.png"
    if ($UpdateGolden -or -not (Test-Path $ref)) {
        $bmp = [Drawing.Image]::FromFile($img); $bmp.Save($ref, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        Write-Host "[new ] golden image $name.png"
        continue
    }
    $r = [ImgCmp]::Compare($img, $ref)
    $ok = ($r[0] -le 2.0) -and ($r[1] -le 0.005)
    if (-not $ok) { $failed++ }
    "{0} render {1,-15} mean diff {2:N3}  pixels off {3:P2}" -f ($(if ($ok) {"[ ok ]"} else {"[FAIL]"})), $name, $r[0], $r[1]
}

# ---- 3. shadow size vs theory ----
# spin 0, static observer at the default 11.81 rs: sin(a) = (3*sqrt(3)/2)(rs/r) sqrt(1 - rs/r)
# -> 134.2 px from the centre at 720 px tall with a 60 degree field of view
$img = Render "shadow" @("--spin","0","--flat-disk","--no-stars","--no-bloom","--grid","off") "1280x720"
$d = @([ImgCmp]::EdgeDistance($img, 0, -1), [ImgCmp]::EdgeDistance($img, -1, 0), [ImgCmp]::EdgeDistance($img, 1, 0))
$worst = ($d | ForEach-Object { [math]::Abs($_ - 134.2) } | Measure-Object -Maximum).Maximum
$ok = $worst -le 2.0
if (-not $ok) { $failed++ }
"{0} shadow radius px (up/left/right) {1} vs 134.2 theory" -f ($(if ($ok) {"[ ok ]"} else {"[FAIL]"})), ($d -join "/")

if ($failed -gt 0) { Write-Host "`n$failed check(s) failed"; exit 1 }
Write-Host "`nall checks passed"
