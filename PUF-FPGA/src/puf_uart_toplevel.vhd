library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

-- Top-level implementation of PUF with UART interface for Basys2

entity puf_uart_toplevel is
    Port ( 
        clk : in STD_LOGIC;
        rx : in STD_LOGIC;       -- UART RX input
        tx : out STD_LOGIC;      -- UART TX output
        leds : out STD_LOGIC_VECTOR(7 downto 0)  -- Debug LEDs
        -- ws2812_din : out STD_LOGIC  -- WS2812 running indicator
    );
end puf_uart_toplevel;

architecture Behavioral of puf_uart_toplevel is

    -- Component declarations
    component uart_receiver is
        generic (
            c_clkfreq : integer := 27_000_000;
            c_baudrate : integer := 115_200
        );
        port (
            clk : in std_logic;
            rx_i : in std_logic;
            read_ready : in std_logic;
            payload_data : out std_logic_vector(63 downto 0);
            payload_valid : out std_logic;
            leds_o : out std_logic_vector (7 downto 0)
        );
    end component;

    component uart_transmitter is
        generic (
            c_clkfreq : INTEGER := 27_000_000;
            c_baudrate : INTEGER := 115_200;
            c_stopbit : INTEGER := 1
        );
        port (
            clk : in STD_LOGIC;
            start_i : in STD_LOGIC;
            data_i : in STD_LOGIC_VECTOR(7 downto 0);
            tx_o : out STD_LOGIC;
            tx_done_tick : out STD_LOGIC;
            busy_o : out STD_LOGIC;
            write_ready : out STD_LOGIC
        );
    end component;

    component uart_puf_interface is
        Port ( 
            clk : in STD_LOGIC;
            payload_data : in STD_LOGIC_VECTOR(63 downto 0);
            payload_valid : in STD_LOGIC;
            puf_payload : out STD_LOGIC_VECTOR(63 downto 0);
            puf_read_valid : out STD_LOGIC;
            puf_read_ready : in STD_LOGIC;
            puf_write_valid : in STD_LOGIC;
            puf_write_ready : out STD_LOGIC;
            puf_response : in STD_LOGIC_VECTOR(63 downto 0);
            tx_start : out STD_LOGIC;
            tx_data : out STD_LOGIC_VECTOR(7 downto 0);
            tx_busy : in STD_LOGIC;
            tx_write_ready : in STD_LOGIC
        );
    end component;

    component PUF_controller is
        Port ( 
            clk : in STD_LOGIC;
            payload : in STD_LOGIC_VECTOR(63 downto 0);
            read_valid : in STD_LOGIC;
            write_ready : in STD_LOGIC;
            read_ready : out STD_LOGIC;
            write_valid : out STD_LOGIC;
            ff_reset : out STD_LOGIC;
            ASR_enable : out STD_LOGIC;
            ASR_tuning : out STD_LOGIC_VECTOR(11 downto 0);
            ASR_choice : out STD_LOGIC_VECTOR(3 downto 0)
        );
    end component;

    component CHOICE_PUF_gen is
        generic(
            PUF_WIDTH : integer := 32
        );
        Port ( 
            clk : in STD_LOGIC;
            ff_reset : in STD_LOGIC;
            chip_enable : in STD_LOGIC;
            ASR_length_conf : in STD_LOGIC_VECTOR(11 downto 0);
            ASR_data_conf : in STD_LOGIC_VECTOR(3 downto 0);
            puf_response : out STD_LOGIC_VECTOR((PUF_WIDTH - 1) downto 0)
        );
    end component;

    component ws2812_driver is
        generic (
            CLK_FREQ     : integer := 27_000_000;
            WS2812_COLOR : STD_LOGIC_VECTOR(23 downto 0) := x"010000"
        );
        port (
            clk  : in  STD_LOGIC;
            dout : out STD_LOGIC
        );
    end component;

    -- Internal signals
    -- UART Receiver → Interface
    signal uart_rx_payload : STD_LOGIC_VECTOR(63 downto 0);
    signal uart_rx_valid : STD_LOGIC;
    signal uart_rx_leds : STD_LOGIC_VECTOR(7 downto 0);
    
    -- Interface → UART Transmitter
    signal uart_tx_start : STD_LOGIC;
    signal uart_tx_data : STD_LOGIC_VECTOR(7 downto 0);
    signal uart_tx_busy : STD_LOGIC;
    signal uart_tx_write_ready : STD_LOGIC;
    
    -- Interface ↔ PUF Controller
    signal puf_payload : STD_LOGIC_VECTOR(63 downto 0);
    signal puf_read_valid : STD_LOGIC;
    signal puf_read_ready : STD_LOGIC;
    signal puf_write_valid : STD_LOGIC;
    signal puf_write_ready : STD_LOGIC;
    signal puf_response : STD_LOGIC_VECTOR(63 downto 0) := (others => '0');
    
    -- PUF Controller → CHOICE_PUF_gen
    signal ff_reset : STD_LOGIC;
    signal asr_enable : STD_LOGIC;
    signal asr_tuning : STD_LOGIC_VECTOR(11 downto 0);
    signal asr_choice : STD_LOGIC_VECTOR(3 downto 0);

    -- LED Fader Signals (PWM for leds(0))
    signal pwm_counter : unsigned(7 downto 0) := (others => '0');
    signal fade_counter : unsigned(7 downto 0) := (others => '0');
    signal fade_dir : std_logic := '1'; -- 1 for up, 0 for down
    signal prescaler : unsigned(16 downto 0) := (others => '0'); -- ~206Hz fade update rate for very slow blink

begin

    -- UART Receiver Instance

    -- UART Receiver Instance
    uart_rx_inst : uart_receiver
        generic map (
            c_clkfreq => 27_000_000,
            c_baudrate => 115_200
        )
        port map (
            clk => clk,
            rx_i => rx,
            read_ready => puf_read_ready,
            payload_data => uart_rx_payload,
            payload_valid => uart_rx_valid,
            leds_o => uart_rx_leds
        );

    -- UART Transmitter Instance
    uart_tx_inst : uart_transmitter
        generic map (
            c_clkfreq => 27_000_000,
            c_baudrate => 115_200,
            c_stopbit => 1
        )
        port map (
            clk => clk,
            start_i => uart_tx_start,
            data_i => uart_tx_data,
            tx_o => tx,
            tx_done_tick => open,
            busy_o => uart_tx_busy,
            write_ready => uart_tx_write_ready
        );

    -- UART ↔ PUF Interface Instance
    interface_inst : uart_puf_interface
        port map (
            clk => clk,
            payload_data => uart_rx_payload,
            payload_valid => uart_rx_valid,
            puf_payload => puf_payload,
            puf_read_valid => puf_read_valid,
            puf_read_ready => puf_read_ready,
            puf_write_valid => puf_write_valid,
            puf_write_ready => puf_write_ready,
            puf_response => puf_response,
            tx_start => uart_tx_start,
            tx_data => uart_tx_data,
            tx_busy => uart_tx_busy,
            tx_write_ready => uart_tx_write_ready
        );

    -- PUF Controller Instance
    puf_ctrl_inst : PUF_controller
        port map (
            clk => clk,
            payload => puf_payload,
            read_valid => puf_read_valid,
            write_ready => puf_write_ready,
            read_ready => puf_read_ready,
            write_valid => puf_write_valid,
            ff_reset => ff_reset,
            ASR_enable => asr_enable,
            ASR_tuning => asr_tuning,
            ASR_choice => asr_choice
        );

    -- CHOICE_PUF_gen Instance (7-bit PUF)
    puf_gen_inst : CHOICE_PUF_gen
        generic map (
            PUF_WIDTH => 64
        )
        port map (
            clk => clk,
            ff_reset => ff_reset,
            chip_enable => asr_enable,
            ASR_length_conf => asr_tuning,
            ASR_data_conf => asr_choice,
            puf_response => puf_response(63 downto 0)
        );

    -- Debug output: show PUF response on LEDs (bits 7:1)
    leds(7 downto 1) <= puf_response(7 downto 1);

    -- Slow fading LED(0) running indicator via PWM
    process(clk)
    begin
        if rising_edge(clk) then
            -- Increment high-speed PWM counter for modulating brightness
            pwm_counter <= pwm_counter + 1;
            
            -- Turn LED on if pwm counter is less than current fade level
            if pwm_counter < fade_counter then
                leds(0) <= '1';
            else
                leds(0) <= '0';
            end if;

            -- Update fade level at a slow rate
            prescaler <= prescaler + 1;
            if prescaler = 0 then -- Overflow acts as a slow tick
                if fade_dir = '1' then
                    if fade_counter = 255 then
                        fade_dir <= '0';
                    else
                        fade_counter <= fade_counter + 1;
                    end if;
                else
                    if fade_counter = 0 then
                        fade_dir <= '1';
                    else
                        fade_counter <= fade_counter - 1;
                    end if;
                end if;
            end if;
        end if;
    end process;

end Behavioral;
