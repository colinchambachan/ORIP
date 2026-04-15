## Results

### Spin vs Coalesce


| Metric              | Spin | Coalesce |
| ------------------- | ---- | -------- |
| Applied Events      | 3000 | ~1333    |
| Queue Full Hits     | 266  | 1679     |
| Pending Overwrites  | 0    | 1667     |
| Final State Correct | Yes  | Yes      |


Coalescing reduced processed work by ~55% while preserving correctness.