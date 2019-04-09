if [ $# -ne 2 ]; then
  echo "LFSUnlock <Starting ID> <Ending ID>"
  exit 1
fi

mkfifo tmp

counter=0

for i in $(seq $1 $2)
do
  if [ $counter -lt 50 ]; then
    { git-lfs unlock --force --id=$i; echo 'done' > tmp; } &
    let $[counter++];
  else
    read x < tmp
    { git-lfs unlock --force --id=$i; echo 'done' > tmp; } &
  fi
done

cat ./tmp > /dev/null

rm tmp