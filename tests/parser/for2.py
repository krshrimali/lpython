for i in range(N): # type: parallel
    c[i] = a[i] + scalar * b[i]

i: i32
for i in range(10):
    if 2 > i : pass
    if i > 5 : break
    if i == 5 and i < 10 : i = 3

# Fow showing the parsing order of 'in' in for-loop and 'in' in expr is correct
# and there is no conflict. Otherwise, the code below is gibberish.
for i in a in list1:
    pass

for item in list1:
    if item in list2:
        pass

if a in list1 and b not in list2 or c in list3:
    pass
