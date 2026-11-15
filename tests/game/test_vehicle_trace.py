"""The occupied-car recorder keeps ordinary ticks in RAM and saves a real pre-trigger history."""
import json
import time
from pathlib import Path


def test_vehicle_trace_trigger(g):
    car = g.player()['vehicle']['actor']
    body = g.call('vehicle.body', car=car)
    g.later('vehicle.body', car=car, pos=body['pos'], velocity=body['velocity'], turn=body['turn'])
    baseline = g.call('vehicle.trace')
    g.sleep(350)
    idle = g.call('vehicle.trace')
    g.check('ordinary car ticks are buffered without an automatic write',
            idle['supported'] and idle['samples'] > baseline['samples'] and
            idle['automatic_captures'] == baseline['automatic_captures'] and idle['buffer_bytes'] < 192 * 1024,
            idle)
    g.call('vehicle.body', car=car, velocity=[0, 25, 0])
    deadline = time.monotonic() + 2
    state = g.call('vehicle.trace')
    while state['automatic_captures'] == idle['automatic_captures'] and time.monotonic() < deadline:
        time.sleep(.025)
        state = g.call('vehicle.trace')
    assert state['automatic_captures'] == idle['automatic_captures'] + 1, state
    path = Path(state['file'])
    data = json.loads(path.read_text(encoding='utf-8'))
    samples = data['samples']
    first, last = samples[0], samples[-1]
    g.check('trigger saves the correct car and the preceding quiet ticks',
            data['car'].lower() == car.lower() and data['trigger'] != 'manual' and
            4 < len(samples) <= 512 and first['ms_ago'] >= 250 and abs(first['before_native']['velocity'][1]) < 1,
            {'car': data['car'], 'reason': data['trigger'], 'samples': len(samples)})
    g.check('trace locates the injected change before the native tick',
            last['before_native']['velocity'][1] > 20 and
            len(last['after_native']['velocity']) == len(last['after_tick']['velocity']) == 3 and
            last['wheels_load_n_compression_m_flags'], last)
    g.call('vehicle.body', car=car, velocity=[0, 0, 0])
    g.call('vehicle.trace', save=True)
    previous = path.with_name('vehicle-trace.prev.json')
    g.check('manual capture preserves the preceding automatic incident',
            json.loads(path.read_text(encoding='utf-8'))['trigger'] == 'manual' and
            json.loads(previous.read_text(encoding='utf-8')) == data)
