import { SerialPort } from 'serialport'
import { ReadlineParser } from '@serialport/parser-readline'
import { readdir } from 'node:fs'
import debug from 'debug'

const V = true
const BAUD = 115200
const ports = []

const ESP_LOG_FMT = /([IDWE]) \((\d+)\) ([^:]+): (.+)\x1B/

function open (file) {
  const node = parseInt(file.match(/(\d+)$/)[1])
  let clock = 0
  let status = 'OFFLINE'
  let ndi = '--:--:--:--:--:--'
  console.info(`Opening ${file}`)
  const port = new SerialPort({ path: file, baudRate: BAUD })
  const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }))
  parser.on('data', forward)
  const log = debug(`NODE#${node}`)
  if (V) log.enabled = true

  function forward (line) {
    const time = Date.now()
    let event = { time, node, level: 'N', source: '_' }
    if (ESP_LOG_FMT.test(line)) {
      const [_, level, ticks, source, message] = line.match(ESP_LOG_FMT)
      clock = ticks
      event = { ...event, level, source, message }

      const SNAIL_STATUS_FMT = /Status change: (\w+) => (\w+), v: (\d)/
      if (source === 'snail.c' && SNAIL_STATUS_FMT.test(message)) {
        const [_, prev, next, success] = message.match(SNAIL_STATUS_FMT)
        if (success === '0') status = next
      }

      const NAN_OWN_NDI = /own_ndi: ([0-9A-F]{2}:[0-9A-F]{2}:[0-9A-F]{2}:[0-9A-F]{2}:[0-9A-F]{2}:[0-9A-F]{2})/i
      if (source === 'nanr.c' && NAN_OWN_NDI.test(message)) {
        const [_, mac] = message.match(NAN_OWN_NDI)
        ndi = mac
      }
    } else {
      event.message = line
    }
    event = {...event, clock, status, ndi }
    if (V) log(`${status[0]}> ${event.level} (${event.source}) ${event.message}`)
  }
}


process.on('SIGINT', () => {
  for (const port of ports) port.close()
  console.info('ttys closed')
  process.exit(0)
});

const dir = '/dev'
const devices = (await new Promise((resolve, reject) => readdir(dir, { withFileTypes: true }, (err, res) => err ? reject(err) : resolve(res))))
  .filter(ent => /^ttyUSB\d+$/.test(ent.name))

for (const ent of devices) {
  const dev = dir + '/' + ent.name
  open(dev)
}

