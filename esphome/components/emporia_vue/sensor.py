from esphome.components import sensor, i2c
from esphome import automation
import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.helpers import sanitize, snake_case
import re

from esphome.const import (
    CONF_CALIBRATION,
    CONF_NAME,
    CONF_CURRENT,
    CONF_ID,
    CONF_INPUT,
    CONF_POWER,
    CONF_TRIGGER_ID,
    CONF_VOLTAGE,
    CONF_FREQUENCY,
    CONF_PHASE_ANGLE,
    DEVICE_CLASS_APPARENT_POWER,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER_FACTOR,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_VOLT_AMPS,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_AMPERE,
    UNIT_WATT,
    UNIT_VOLT,
    UNIT_HERTZ,
    UNIT_DEGREES,
)

CONF_CT_CLAMPS = "ct_clamps"
CONF_PHASES = "phases"
CONF_PHASE_ID = "phase_id"
CONF_VARIANT = "variant"

CONF_ON_UPDATE = "on_update"
CONF_ON_LOAD_EVENT = "on_load_event"
CONF_CIRCUITS = "circuits"
CONF_INPUTS = "inputs"
CONF_KEY = "key"
CONF_REVERSE = "reverse"
CONF_APPARENT_POWER = "apparent_power"
CONF_POWER_FACTOR = "power_factor"
CONF_PUBLISH_INTERVAL = "publish_interval"
CONF_FRAME_RATE = "frame_rate"
CONF_MISSED_FRAMES = "missed_frames"
CONF_LOAD_EVENTS = "load_events"
CONF_STEP_DETECTION = "step_detection"
CONF_NOISE_FACTOR = "noise_factor"
CONF_MIN_STEP = "min_step"
CONF_MIN_STEP_PERCENT = "min_step_percent"
CONF_SETTLE_WITHIN = "settle_within"
CONF_SETTLE_WITHIN_PERCENT = "settle_within_percent"
CONF_CONFIRM_FRAMES = "confirm_frames"
CONF_SETTLE_FRAMES = "settle_frames"
CONF_TIMEOUT = "timeout"

CODEOWNERS = ["@flaviut", "@Maelstrom96", "@krconv"]
ESP_PLATFORMS = ["esp-idf"]
DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor"]

emporia_vue_ns = cg.esphome_ns.namespace("emporia_vue")
EmporiaVueComponent = emporia_vue_ns.class_(
    "EmporiaVueComponent", cg.PollingComponent, i2c.I2CDevice
)
PhaseConfig = emporia_vue_ns.class_("PhaseConfig")
CTClampConfig = emporia_vue_ns.class_("CTClampConfig")
CircuitConfig = emporia_vue_ns.class_("CircuitConfig")
LoadEvent = emporia_vue_ns.struct("LoadEvent")
StepConfig = emporia_vue_ns.struct("StepConfig")

# Trigger for after statistics sensors are updated
EmporiaVueUpdateTrigger = emporia_vue_ns.class_(
    "EmporiaVueUpdateTrigger", automation.Trigger.template()
)
# Trigger for a finished step on a clamp or circuit that watches for them
EmporiaVueLoadEventTrigger = emporia_vue_ns.class_(
    "EmporiaVueLoadEventTrigger",
    automation.Trigger.template(LoadEvent.operator("const").operator("ref")),
)


PhaseInputWire = emporia_vue_ns.enum("PhaseInputWire")
PHASE_INPUT = {
    "BLACK": PhaseInputWire.BLACK,
    "RED": PhaseInputWire.RED,
    "BLUE": PhaseInputWire.BLUE,
}

CTInputPort = emporia_vue_ns.enum("CTInputPort")
CT_INPUT = {
    "A": CTInputPort.A,
    "B": CTInputPort.B,
    "C": CTInputPort.C,
    "1": CTInputPort.ONE,
    "2": CTInputPort.TWO,
    "3": CTInputPort.THREE,
    "4": CTInputPort.FOUR,
    "5": CTInputPort.FIVE,
    "6": CTInputPort.SIX,
    "7": CTInputPort.SEVEN,
    "8": CTInputPort.EIGHT,
    "9": CTInputPort.NINE,
    "10": CTInputPort.TEN,
    "11": CTInputPort.ELEVEN,
    "12": CTInputPort.TWELVE,
    "13": CTInputPort.THIRTEEN,
    "14": CTInputPort.FOURTEEN,
    "15": CTInputPort.FIFTEEN,
    "16": CTInputPort.SIXTEEN,
}

MAINS_INPUTS = {"A", "B", "C"}

watts = cv.float_with_unit("power", "(w|W)?")

SCHEMA_MEASURED = {
    cv.Optional(CONF_POWER): sensor.sensor_schema(
        unit_of_measurement=UNIT_WATT,
        device_class=DEVICE_CLASS_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    cv.Optional(CONF_CURRENT): sensor.sensor_schema(
        unit_of_measurement=UNIT_AMPERE,
        device_class=DEVICE_CLASS_CURRENT,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=2,
    ),
    cv.Optional(CONF_APPARENT_POWER): sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT_AMPS,
        device_class=DEVICE_CLASS_APPARENT_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    cv.Optional(CONF_POWER_FACTOR): sensor.sensor_schema(
        device_class=DEVICE_CLASS_POWER_FACTOR,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=2,
    ),
    # The name its load events carry; defaults to the power sensor's name without
    # "Power" (Hot Tub Power -> hot_tub).
    cv.Optional(CONF_KEY): cv.string_strict,
    # Whether to watch it for load events (see on_load_event).
    cv.Optional(CONF_LOAD_EVENTS): cv.boolean,
}

SCHEMA_CT_CLAMP = {
    cv.GenerateID(): cv.declare_id(CTClampConfig),
    cv.Required(CONF_PHASE_ID): cv.use_id(PhaseConfig),
    cv.Required(CONF_INPUT): cv.enum(CT_INPUT),
    # For a clamp fitted the wrong way round.
    cv.Optional(CONF_REVERSE, default=False): cv.boolean,
    **SCHEMA_MEASURED,
}

SCHEMA_CIRCUIT = {
    cv.GenerateID(): cv.declare_id(CircuitConfig),
    # The clamps measuring it, by input; each must also be listed under ct_clamps.
    cv.Required(CONF_INPUTS): cv.All(
        cv.ensure_list(cv.enum(CT_INPUT)), cv.Length(min=2, max=3)
    ),
    **SCHEMA_MEASURED,
}

SCHEMA_STEP_DETECTION = cv.Schema(
    {
        cv.Optional(CONF_MIN_STEP, default="30W"): watts,
        cv.Optional(CONF_MIN_STEP_PERCENT, default="2%"): cv.percentage,
        cv.Optional(CONF_NOISE_FACTOR, default=4.0): cv.float_range(min=0, max=20),
        cv.Optional(CONF_SETTLE_WITHIN, default="10W"): watts,
        cv.Optional(CONF_SETTLE_WITHIN_PERCENT, default="3%"): cv.percentage,
        cv.Optional(CONF_CONFIRM_FRAMES, default=2): cv.int_range(min=1, max=8),
        cv.Optional(CONF_SETTLE_FRAMES, default=4): cv.int_range(min=2, max=8),
        cv.Optional(CONF_TIMEOUT, default="10s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(max=cv.TimePeriod(seconds=60)),
        ),
    }
)


def _slug(text):
    # The object id ESPHome gives an entity of that name (its MQTT topic's).
    return sanitize(snake_case(str(text)))


def _key(item, fallback):
    if CONF_KEY in item:
        return item[CONF_KEY]
    if CONF_POWER in item and CONF_NAME in item[CONF_POWER]:
        # "Hot Tub Power" -> "hot_tub", the name its power sensor's object id starts with
        return re.sub(r"_power$", "", _slug(item[CONF_POWER][CONF_NAME])) or "power"
    return fallback


def validate_phases(val):
    base_validated = cv.Schema(
        cv.ensure_list(
            {
                cv.Required(CONF_ID): cv.declare_id(PhaseConfig),
                cv.Required(CONF_INPUT): cv.enum(PHASE_INPUT),
                cv.Optional(CONF_CALIBRATION, default=0.022): cv.zero_to_one_float,
                cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
                    unit_of_measurement=UNIT_VOLT,
                    device_class=DEVICE_CLASS_VOLTAGE,
                    state_class=STATE_CLASS_MEASUREMENT,
                    accuracy_decimals=1,
                ),
                cv.Optional(CONF_FREQUENCY): sensor.sensor_schema(
                    unit_of_measurement=UNIT_HERTZ,
                    device_class=DEVICE_CLASS_FREQUENCY,
                    state_class=STATE_CLASS_MEASUREMENT,
                    accuracy_decimals=1,
                ),
                cv.Optional(CONF_PHASE_ANGLE): sensor.sensor_schema(
                    unit_of_measurement=UNIT_DEGREES,
                    state_class=STATE_CLASS_MEASUREMENT,
                    accuracy_decimals=0,
                ),
            }
        )
    )(val)

    if len(base_validated) > 3:
        raise cv.Invalid("No more than 3 phases are supported")

    inputs = [phase[CONF_INPUT] for phase in base_validated]
    if len(inputs) != len(set(inputs)):
        raise cv.Invalid("Only one entry per input color is allowed")

    for i, phase in enumerate(base_validated):
        input_wire = phase[CONF_INPUT]
        if input_wire == "BLACK":
            if CONF_PHASE_ANGLE in phase:
                raise cv.Invalid(
                    "Phase angle is not supported for the black wire, only for the "
                    "red and blue wires",
                    path=[i, CONF_PHASE_ANGLE],
                )
        elif input_wire in {"RED", "BLUE"}:
            if CONF_FREQUENCY in phase:
                raise cv.Invalid(
                    "Frequency is not supported for the red and blue wires, only for "
                    "the black wire",
                    path=[i, CONF_FREQUENCY],
                )

    return base_validated


def validate_channels(config):
    """Circuits name clamps that exist and are in no other circuit; event keys are unique."""
    clamp_inputs = [str(ct[CONF_INPUT]) for ct in config[CONF_CT_CLAMPS]]
    in_circuit = set()
    for i, circuit in enumerate(config.get(CONF_CIRCUITS, [])):
        for j, inp in enumerate(circuit[CONF_INPUTS]):
            path = [CONF_CIRCUITS, i, CONF_INPUTS, j]
            if str(inp) not in clamp_inputs:
                raise cv.Invalid(f"Input {inp} is not listed under ct_clamps", path=path)
            if clamp_inputs.count(str(inp)) > 1:
                raise cv.Invalid(
                    f"Input {inp} is listed more than once under ct_clamps, so the circuit cannot tell which",
                    path=path,
                )
            if str(inp) in in_circuit:
                raise cv.Invalid(f"Input {inp} is already in another circuit", path=path)
            in_circuit.add(str(inp))

    watching = CONF_ON_LOAD_EVENT in config
    keys = {}

    def watch(item, default, fallback, path):
        on = item.get(CONF_LOAD_EVENTS, default)
        if on and not watching:
            raise cv.Invalid(
                "load_events needs an on_load_event automation to report them to",
                path=path + [CONF_LOAD_EVENTS],
            )
        if not on:
            return
        key = _key(item, fallback)
        if key in keys:
            raise cv.Invalid(
                f"Load event key '{key}' is used twice (set a key: to tell them apart)",
                path=path,
            )
        keys[key] = path

    for i, ct in enumerate(config[CONF_CT_CLAMPS]):
        inp = str(ct[CONF_INPUT])
        default = watching and inp not in MAINS_INPUTS and inp not in in_circuit
        watch(ct, default, f"input_{inp}", [CONF_CT_CLAMPS, i])
    for i, circuit in enumerate(config.get(CONF_CIRCUITS, [])):
        inputs = "_".join(str(inp) for inp in circuit[CONF_INPUTS])
        watch(circuit, watching, f"inputs_{inputs}", [CONF_CIRCUITS, i])
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EmporiaVueComponent),
            cv.Required(CONF_PHASES): validate_phases,
            cv.Required(CONF_CT_CLAMPS): cv.ensure_list(SCHEMA_CT_CLAMP),
            cv.Optional(CONF_CIRCUITS): cv.ensure_list(SCHEMA_CIRCUIT),
            cv.Optional(CONF_VARIANT, default="vue2"): cv.one_of("vue2", "vue3", lower=True),
            # Publish each sensor's mean over this span instead of every frame.
            cv.Optional(CONF_PUBLISH_INTERVAL): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_FRAME_RATE): sensor.sensor_schema(
                unit_of_measurement=UNIT_HERTZ,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                accuracy_decimals=2,
                icon="mdi:speedometer",
            ),
            cv.Optional(CONF_MISSED_FRAMES): sensor.sensor_schema(
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                accuracy_decimals=0,
                icon="mdi:alert-circle-outline",
            ),
            cv.Optional(CONF_STEP_DETECTION, default={}): SCHEMA_STEP_DETECTION,
            cv.Optional(CONF_ON_UPDATE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        EmporiaVueUpdateTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_LOAD_EVENT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        EmporiaVueLoadEventTrigger
                    ),
                }
            ),
        },
    )
    .extend(cv.polling_component_schema("200ms"))
    .extend(i2c.i2c_device_schema(0x64)),
    cv.only_on_esp32,
    validate_channels,
)


async def _measured(var, item, step_config, watching, default, fallback):
    for conf, setter in (
        (CONF_POWER, var.set_power_sensor),
        (CONF_CURRENT, var.set_current_sensor),
        (CONF_APPARENT_POWER, var.set_apparent_power_sensor),
        (CONF_POWER_FACTOR, var.set_power_factor_sensor),
    ):
        if conf in item:
            cg.add(setter(await sensor.new_sensor(item[conf])))
    if watching and item.get(CONF_LOAD_EVENTS, default):
        cg.add(var.set_load_events(_key(item, fallback), step_config))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if config.get(CONF_VARIANT, "vue2") == "vue3":
        cg.add_define("EMPORIA_VUE_VARIANT_VUE3")
    else:
        cg.add_define("EMPORIA_VUE_VARIANT_VUE2")

    phases = []
    for phase_config in config[CONF_PHASES]:
        phase_var = cg.new_Pvariable(phase_config[CONF_ID], PhaseConfig())
        cg.add(phase_var.set_input_wire(phase_config[CONF_INPUT]))
        cg.add(phase_var.set_calibration(phase_config[CONF_CALIBRATION]))

        if CONF_VOLTAGE in phase_config:
            voltage_sensor = await sensor.new_sensor(phase_config[CONF_VOLTAGE])
            cg.add(phase_var.set_voltage_sensor(voltage_sensor))

        if CONF_FREQUENCY in phase_config:
            frequency_sensor = await sensor.new_sensor(phase_config[CONF_FREQUENCY])
            cg.add(phase_var.set_frequency_sensor(frequency_sensor))

        if CONF_PHASE_ANGLE in phase_config:
            phase_angle_sensor = await sensor.new_sensor(phase_config[CONF_PHASE_ANGLE])
            cg.add(phase_var.set_phase_angle_sensor(phase_angle_sensor))

        phases.append(phase_var)
    cg.add(var.set_phases(phases))

    if CONF_PUBLISH_INTERVAL in config:
        cg.add(var.set_publish_interval(config[CONF_PUBLISH_INTERVAL]))
    if CONF_FRAME_RATE in config:
        cg.add(var.set_frame_rate_sensor(await sensor.new_sensor(config[CONF_FRAME_RATE])))
    if CONF_MISSED_FRAMES in config:
        cg.add(var.set_missed_frames_sensor(await sensor.new_sensor(config[CONF_MISSED_FRAMES])))

    events = config[CONF_STEP_DETECTION]
    watching = CONF_ON_LOAD_EVENT in config
    step_config = cg.StructInitializer(
        StepConfig,
        ("min_step_w", events[CONF_MIN_STEP]),
        ("rel_step", events[CONF_MIN_STEP_PERCENT]),
        ("noise_factor", events[CONF_NOISE_FACTOR]),
        ("settle_w", events[CONF_SETTLE_WITHIN]),
        ("settle_rel", events[CONF_SETTLE_WITHIN_PERCENT]),
        ("confirm", events[CONF_CONFIRM_FRAMES]),
        ("settle_frames", events[CONF_SETTLE_FRAMES]),
        ("timeout_ms", events[CONF_TIMEOUT].total_milliseconds),
    )

    in_circuit = {str(inp) for c in config.get(CONF_CIRCUITS, []) for inp in c[CONF_INPUTS]}
    ct_clamps = []
    by_input = {}
    for ct_config in config[CONF_CT_CLAMPS]:
        ct_clamp_var = cg.new_Pvariable(ct_config[CONF_ID], CTClampConfig())
        phase_var = await cg.get_variable(ct_config[CONF_PHASE_ID])
        cg.add(ct_clamp_var.set_phase(phase_var))
        cg.add(ct_clamp_var.set_input_port(ct_config[CONF_INPUT]))
        if ct_config[CONF_REVERSE]:
            cg.add(ct_clamp_var.set_reverse(True))

        inp = str(ct_config[CONF_INPUT])
        default = inp not in MAINS_INPUTS and inp not in in_circuit
        await _measured(ct_clamp_var, ct_config, step_config, watching, default, f"input_{inp}")

        ct_clamps.append(ct_clamp_var)
        by_input[inp] = ct_clamp_var
    cg.add(var.set_ct_clamps(ct_clamps))

    circuits = []
    for circuit_config in config.get(CONF_CIRCUITS, []):
        circuit_var = cg.new_Pvariable(circuit_config[CONF_ID], CircuitConfig())
        inputs = [str(inp) for inp in circuit_config[CONF_INPUTS]]
        cg.add(circuit_var.set_clamps([by_input[inp] for inp in inputs]))
        await _measured(circuit_var, circuit_config, step_config, watching, True, "inputs_" + "_".join(inputs))
        circuits.append(circuit_var)
    if circuits:
        cg.add(var.set_circuits(circuits))

    for trigger_conf in config.get(CONF_ON_UPDATE, []):
        trigger = cg.new_Pvariable(trigger_conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], trigger_conf )

    for trigger_conf in config.get(CONF_ON_LOAD_EVENT, []):
        trigger = cg.new_Pvariable(trigger_conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(LoadEvent.operator("const").operator("ref"), "event")], trigger_conf
        )
