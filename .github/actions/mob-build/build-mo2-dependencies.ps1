param(
    [Parameter(Mandatory=$true)][String]$Owner,
    [Parameter(Mandatory=$true)][String]$Branch,
    [Parameter(Mandatory=$true)][String]$DependenciesS
)

# Exact upstream commits as of the MO2 2.5.2 release (2024-08-04). Without
# this, mob checks out the 2026 master branch of every dependency, whose CMake
# (e.g. find_package(7zip CONFIG REQUIRED)) no longer matches mob v2.5.2.
$Pins = @{
    "usvfs"          = "9f7fd9660d51784aa2117cb45f2095e87312d558"
    "cmake_common"   = "8abcb29e0810e07f9a464458264b904e9017e633"
    "uibase"         = "44201d70f7c1a6cda55da86458dc5b8b4665c47b"
    "githubpp"       = "308c00967d1237e1ff8589f8aa36ad8b016a662a"
    "bsatk"          = "2882fd352bdd97d4bc67bedafeba6d55c269cdeb"
    "esptk"          = "9d9708bc827fdfa9019e24e0bdd3ed7d35d1553e"
    "archive"        = "a13e224c17fb0f2210305cb3dcc442f25f2fd58c"
    "lootcli"        = "460a29d3fa4c260db192c18741ac8dd21a549ae3"
    "game_gamebryo"  = "0076e5431bd7fffb4977c724a92027e6e4f11f2e"
}

function Pin-Repo {
    param ([System.IO.FileSystemInfo]$Folder, [String]$Sha)
    Push-Location $Folder
    # mob may have made a shallow clone; the 2024 commit is then unavailable.
    git fetch --unshallow origin 2>&1 | Out-Null
    git fetch origin $Sha 2>&1 | Out-Null
    git cat-file -e "$Sha^{commit}" 2>&1 | Out-Null
    if ($LASTEXITCODE) {
        Write-Output "WARNING: $($Folder.Name): pinned commit $Sha unavailable, keeping current HEAD"
    }
    else {
        # mob switches to 'master' during its build; point that local branch at
        # the pinned commit so the second mob invocation cannot undo the pin.
        git checkout -B master $Sha --force 2>&1 | Out-Null
        if ($LASTEXITCODE) {
            Write-Output "WARNING: $($Folder.Name): checkout of $Sha failed"
        }
        else {
            Write-Output "pinned $($Folder.Name) -> $($Sha.Substring(0,12))"
        }
    }
    Pop-Location
}

$Dependencies = $DependenciesS.Split()

Write-Host "Initializing repositories with mob... "
mob -l 4 -d . build `
    --ignore-uncommitted-changes `
    --redownload --reextract --no-build-task `
    @Dependencies

$dirs = @()
foreach ($p in @("build\usvfs", "build\cmake_common")) {
    if (Test-Path $p) { $dirs += Get-Item $p }
}
if (Test-Path "build\modorganizer_super") {
    $dirs += Get-ChildItem "build\modorganizer_super" -Directory -Exclude ".git"
}
foreach ($d in $dirs) {
    if ($Pins.ContainsKey($d.Name)) { Pin-Repo -Folder $d -Sha $Pins[$d.Name] }
}

Write-Output "Building dependencies with mob... "
mob -l 4 -d . build `
    --ignore-uncommitted-changes `
    --no-pull --reconfigure --rebuild `
    @Dependencies
