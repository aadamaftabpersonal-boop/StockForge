# Run a command inside the Linux dev image with the repo mounted at /work.
#   .\scripts\dev.ps1 make test
# Joins the compose network (if the db is up) and loads .env (gitignored).
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
$img = 'stockforge-dev'
docker image inspect $img *> $null
if ($LASTEXITCODE -ne 0) { docker build -t $img -f deploy/Dockerfile.dev deploy }
$extra = @()
if (Test-Path .env) { $extra += @('--env-file', '.env') }
docker network inspect stockforge_default *> $null
if ($LASTEXITCODE -eq 0) { $extra += @('--network', 'stockforge_default') }
docker run --rm @extra -v "${PWD}:/work" -w /work --cap-add=SYS_PTRACE $img @args
exit $LASTEXITCODE
