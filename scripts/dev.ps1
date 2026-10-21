# Run a command inside the Linux dev image with the repo mounted at /work.
#   .\scripts\dev.ps1 make test
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
$img = 'stockforge-dev'
docker image inspect $img *> $null
if ($LASTEXITCODE -ne 0) { docker build -t $img -f deploy/Dockerfile.dev deploy }
docker run --rm -v "${PWD}:/work" -w /work --cap-add=SYS_PTRACE $img @args
exit $LASTEXITCODE
