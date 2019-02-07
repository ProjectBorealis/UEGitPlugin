if [ $# -ne 2 ]; then
  echo "LFSUnlock <Starting ID> <Ending ID>"
  exit 1
fi

mkfifo tmp

counter=0

for i in {$1..$2}
do
  if [ $counter -lt 1024 ]; then
    { git-lfs unlock --id=$i; echo 'done' > tmp; } &
    let $[counter++];
  else
    read x < tmp
    { git-lfs unlock --id=$i; echo 'done' > tmp; } &
  fi
done

cat /tmp > /dev/null

rm tmp
