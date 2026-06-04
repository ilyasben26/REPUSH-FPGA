library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity ws2812_driver is
    generic (
        CLK_FREQ     : integer := 27_000_000;
        WS2812_COLOR : STD_LOGIC_VECTOR(23 downto 0) := x"000008" -- Dim green (Green value = 16)
    );
    port (
        clk  : in  STD_LOGIC;
        dout : out STD_LOGIC
    );
end entity ws2812_driver;

architecture rtl of ws2812_driver is
    -- WS2812 Protocol Constants (calculated from CLK_FREQ)
    constant WS2812_NUM    : integer := 0;
    constant WS2812_WIDTH  : integer := 24;
    constant DELAY_1_HIGH  : integer := (CLK_FREQ / 1_000_000 * 85 / 100) - 1;
    constant DELAY_1_LOW   : integer := (CLK_FREQ / 1_000_000 * 40 / 100) - 1;
    constant DELAY_0_HIGH  : integer := (CLK_FREQ / 1_000_000 * 40 / 100) - 1;
    constant DELAY_0_LOW   : integer := (CLK_FREQ / 1_000_000 * 85 / 100) - 1;
    constant DELAY_RESET   : integer := (CLK_FREQ / 10) - 1; 
    
    type ws2812_state_type is (ST_RESET, ST_DATA_SEND, ST_BIT_SEND_HIGH, ST_BIT_SEND_LOW);
    signal ws2812_state    : ws2812_state_type := ST_RESET;
    signal ws2812_bit_send : integer range 0 to 511 := 0;
    signal ws2812_data_send: integer range 0 to 511 := 0;
    signal ws2812_clk_count: integer range 0 to 3_000_000 := 0;
    signal ws2812_data_reg : STD_LOGIC_VECTOR(23 downto 0) := (others => '0');

begin
    process(clk)
    begin
        if rising_edge(clk) then
            case ws2812_state is
                when ST_RESET =>
                    dout <= '0';
                    if ws2812_clk_count < DELAY_RESET then
                        ws2812_clk_count <= ws2812_clk_count + 1;
                    else
                        ws2812_clk_count <= 0;
                        ws2812_data_reg <= WS2812_COLOR;
                        ws2812_state <= ST_DATA_SEND;
                    end if;

                when ST_DATA_SEND =>
                    if (ws2812_data_send > WS2812_NUM) and (ws2812_bit_send = WS2812_WIDTH) then
                        ws2812_clk_count <= 0;
                        ws2812_data_send <= 0;
                        ws2812_bit_send <= 0;
                        ws2812_state <= ST_RESET;
                    elsif ws2812_bit_send < WS2812_WIDTH then
                        ws2812_state <= ST_BIT_SEND_HIGH;
                    else
                        ws2812_data_send <= ws2812_data_send + 1;
                        ws2812_bit_send <= 0;
                        ws2812_state <= ST_BIT_SEND_HIGH;
                    end if;

                when ST_BIT_SEND_HIGH =>
                    dout <= '1';
                    if ws2812_data_reg(ws2812_bit_send) = '1' then
                        if ws2812_clk_count < DELAY_1_HIGH then
                            ws2812_clk_count <= ws2812_clk_count + 1;
                        else
                            ws2812_clk_count <= 0;
                            ws2812_state <= ST_BIT_SEND_LOW;
                        end if;
                    else
                        if ws2812_clk_count < DELAY_0_HIGH then
                            ws2812_clk_count <= ws2812_clk_count + 1;
                        else
                            ws2812_clk_count <= 0;
                            ws2812_state <= ST_BIT_SEND_LOW;
                        end if;
                    end if;

                when ST_BIT_SEND_LOW =>
                    dout <= '0';
                    if ws2812_data_reg(ws2812_bit_send) = '1' then
                        if ws2812_clk_count < DELAY_1_LOW then
                            ws2812_clk_count <= ws2812_clk_count + 1;
                        else
                            ws2812_clk_count <= 0;
                            ws2812_bit_send <= ws2812_bit_send + 1;
                            ws2812_state <= ST_DATA_SEND;
                        end if;
                    else
                        if ws2812_clk_count < DELAY_0_LOW then
                            ws2812_clk_count <= ws2812_clk_count + 1;
                        else
                            ws2812_clk_count <= 0;
                            ws2812_bit_send <= ws2812_bit_send + 1;
                            ws2812_state <= ST_DATA_SEND;
                        end if;
                    end if;
            end case;
        end if;
    end process;
end architecture rtl;