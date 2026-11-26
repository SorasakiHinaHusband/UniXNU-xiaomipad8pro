# SPDX-License-Identifier: MIT
# Source from the installed-system qual spec. Uses R, rssh, rsudo, pull, verdict and fexpect.
# mic_check_block REPO [raw|noise-reduced] runs after the desktop gate.
# Set MIC_CHECK_REQUEST_ACCESS=1 for an attended first run.

mic_check_gui() {   # STAGE RESULT_SUBDIR TAG [APP ARG ...]
  local stage=$1 name=$2 tag=$3 args rc=0
  shift 3
  printf -v args ' %q' "$@"
  # Open the actual bundle through LaunchServices to give TCC the app's GUI identity.
  # The text-mode rsudo wrapper encloses its command in single quotes. The app's
  # result line survives wrappers that discard exit status, including open -W.
  SSH_T=95 rsudo "
    u=\$(/usr/bin/stat -f %u /dev/console)
    if ! test \"\$u\" -gt 0 || ! test \"\$u\" = \"\$(/usr/bin/stat -f %u \"$stage\")\"; then
      echo MIC_CONSOLE_UID_MISMATCH
      exit 1
    fi
    /bin/launchctl asuser \"\$u\" /usr/bin/sudo -n -u \"#\$u\" -- \
      /usr/bin/open -W -n --stdout \"$stage/$tag.stdout\" --stderr \"$stage/$tag.stderr\" \
        -a \"$stage/PianoMicCheck.app\" --args$args
    printf \"MIC_OPEN_RC=%s\\n\" \"\$?\"
  " </dev/null > "$R/$name/$tag.launch.txt" 2>&1 || rc=1
  rssh "/bin/cat '$stage/$tag.stdout'" </dev/null > "$R/$name/$tag.txt" 2>&1 || rc=1
  rssh "/bin/cat '$stage/$tag.stderr'" </dev/null > "$R/$name/$tag.stderr" 2>&1 || rc=1
  fexpect "$name/$tag.launch.txt" '^MIC_OPEN_RC=0$' "MIC $tag: GUI app launched" || rc=1
  fexpect "$name/$tag.txt" '^result=' "MIC $tag: app returned a result" || rc=1
  return "$rc"
}

mic_check_block() (
  local repo=$1 input=${2:-raw} device name results stage archive sha mode marker label rc=0
  local -a flags
  if [ "${DRY:-0}" = 1 ]; then
    verdict 'INFO MIC: dry run, GUI recording and playback skipped'
    return 0
  fi
  case "$input" in
    raw) device='^input_device=Mic$' ;;
    noise-reduced) device='^input_device=Mic [(]noise reduced[)]$' ;;
    *) verdict "FAIL HARNESS MIC: unknown input choice $input"; return 1 ;;
  esac

  umask 077
  results=$(mktemp -d "$R/mic-check.XXXXXXXX") || return 1
  name=${results##*/}
  verdict "INFO MIC: evidence directory $results"
  archive=$results/app.tgz
  trap 'rm -f -- "$archive"' EXIT
  if ! /usr/bin/codesign --verify --strict "$repo/out/piano-mic-check/PianoMicCheck.app" > "$results/build.txt" 2>&1 ||
     ! COPYFILE_DISABLE=1 /usr/bin/tar -czf "$archive" -C "$repo/out/piano-mic-check" PianoMicCheck.app; then
    verdict "FAIL HARNESS MIC: build and sign PianoMicCheck.app before staging ($name/build.txt)"
    return 1
  fi
  sha=$(/usr/bin/shasum -a 256 < "$archive") || return 1
  sha=${sha%% *}
  stage=$(rssh 'umask 077; d=$(/usr/bin/mktemp -d /private/var/tmp/piano-mic-check.XXXXXXXX) && echo "MIC_STAGE=$d"' </dev/null |
    sed -nE 's@^MIC_STAGE=(/private/var/tmp/piano-mic-check[.][A-Za-z0-9]{8})$@\1@p' | tail -1) || stage=
  if [ -z "$stage" ]; then
    verdict 'FAIL HARNESS MIC: no remote stage directory'
    return 1
  fi
  printf '%s\n' "$stage" > "$results/remote-stage.txt"
  if ! /usr/bin/base64 -i "$archive" | SSH_T=90 rssh "
    umask 077
    cd '$stage' && /usr/bin/base64 -D > app.tgz &&
    sum=\$(/usr/bin/shasum -a 256 < app.tgz) && test \"\${sum%% *}\" = '$sha' &&
    /usr/bin/tar -xzf app.tgz && /usr/bin/codesign --verify --strict PianoMicCheck.app &&
    /bin/rm -f app.tgz && echo MIC_APP_STAGED
  " > "$results/stage.txt" 2>&1 ||
    ! fexpect "$name/stage.txt" '^MIC_APP_STAGED$' 'MIC: signed app and tone staged with verified SHA-256'; then
    verdict "FAIL HARNESS MIC: stage failed ($name/stage.txt)"
    return 1
  fi

  mic_check_gui "$stage" "$name" tcc --tcc || return 1
  local tcc=tcc
  if grep -aqx 'mic_status=0' "$results/tcc.txt" && [ "${MIC_CHECK_REQUEST_ACCESS:-0}" = 1 ]; then
    tcc=permission
    mic_check_gui "$stage" "$name" "$tcc" --request-access || return 1
  fi
  fexpect "$name/$tcc.txt" '^mic_status=3$' 'MIC: console app has microphone permission' || return 1

  for mode in quiet tone; do
    flags=()
    if [ "$mode" = tone ]; then
      flags=(--tone --playback)
    fi
    mic_check_gui "$stage" "$name" "$mode" 4 "$stage/$mode.m4a" ${flags[@]+"${flags[@]}"} || rc=1
    fexpect "$name/$mode.txt" '^result=ok$' "MIC $mode: AVAudioRecorder check completed" || rc=1
    fexpect "$name/$mode.txt" '^mic_status=3$' "MIC $mode: microphone authorized during capture" || rc=1
    fexpect "$name/$mode.txt" "$device" "MIC $mode: selected Piano input is the default device" || rc=1
    fexpect "$name/$mode.txt" '^input_format=[0-9]+ Hz [1-9][0-9]* ch' "MIC $mode: input format reported" || rc=1
    if [ "$mode" = tone ]; then
      fexpect "$name/$mode.txt" '^output_device=Speaker$' 'MIC tone: tablet Speaker is the default output' || rc=1
      fexpect "$name/$mode.txt" '^stimulus_complete=yes$' 'MIC tone: stimulus played through the default output' || rc=1
      fexpect "$name/$mode.txt" '^tone_detected=yes$' 'MIC tone: stimulus found in the saved recording' || rc=1
      fexpect "$name/$mode.txt" '^playback_complete=yes$' 'MIC tone: saved recording played to completion' || rc=1
    fi
    if ! fexpect "$name/$mode.txt" '^recording_published=yes$' "MIC $mode: saved M4A produced" ||
       ! pull "$name/$mode.m4a" "$stage/$mode.m4a" ||
       [ ! -s "$results/$mode.m4a" ]; then
      verdict "FAIL MIC $mode: no non-empty verified recording ($name)"
      rc=1
      continue
    fi
    verdict "PASS MIC $mode: verified non-empty recording ($name/$mode.m4a)"
    python3 "$repo/tooling/native/piano-mic-check/analyze.py" "$results/$mode.m4a" --mode "$mode" --seconds 4 \
      > "$results/$mode.grade.txt" 2>&1 || rc=1
    if [ "$mode" = tone ]; then
      marker='^tone_detected=yes$'
      label='MIC tone: host detects the stimulus in the pulled M4A'
    else
      marker='^noise_floor_sane=yes$'
      label='MIC quiet: host measures a sane noise floor'
    fi
    fexpect "$name/$mode.grade.txt" "$marker" "$label" || rc=1
    fexpect "$name/$mode.grade.txt" '^grade=pass$' "MIC $mode: host grade passed" || rc=1
  done
  return "$rc"
)
