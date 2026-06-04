library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity CHOICE_PUF is
  Port ( 
         clk                  : in STD_LOGIC;
         chip_enable          : in STD_LOGIC;
         ff_reset             : in STD_LOGIC;
         ASR_length_conf      : in STD_LOGIC_VECTOR(11 downto 0);
         ASR_data_conf        : in STD_LOGIC_VECTOR(3 downto 0);
         puf_bit              : out STD_LOGIC
       );
end CHOICE_PUF;

architecture Behavioral of CHOICE_PUF is

  -- PUF signals
  signal carry_out : STD_LOGIC;
  signal ASRQ0, ASRQ1, ASRQ2, ASRQ3 : STD_LOGIC;
  
  -- Shift register signals for each stage
  type sr_array is array (0 to 3) of STD_LOGIC_VECTOR(7 downto 0);
  signal shift_regs : sr_array := (others => (others => '0'));
  
  -- Prevent optimization of structural delay paths only
  attribute syn_dont_touch : integer;
  attribute syn_dont_touch of carry_out : signal is 1;
  attribute syn_dont_touch of ASRQ0 : signal is 1;
  attribute syn_dont_touch of ASRQ1 : signal is 1;
  attribute syn_dont_touch of ASRQ2 : signal is 1;
  attribute syn_dont_touch of ASRQ3 : signal is 1;
  
  attribute syn_preserve : boolean;
  attribute syn_preserve of alu_inst_0 : label is true;
  attribute syn_preserve of alu_inst_1 : label is true;
  attribute syn_preserve of alu_inst_2 : label is true;
  attribute syn_preserve of alu_inst_3 : label is true;

  -- Carry chain signals
  signal c_out_0, c_out_1, c_out_2, c_out_3 : STD_LOGIC;
  signal sum_0, sum_1, sum_2, sum_3 : STD_LOGIC;

  -- Hardware ALU Carry Chain Array (replaces behavioral representation)
  component ALU
    generic (ALU_MODE : integer := 0);
    port (
      COUT : out STD_LOGIC;
      SUM  : out STD_LOGIC;
      I0   : in STD_LOGIC;
      I1   : in STD_LOGIC;
      I3   : in STD_LOGIC;
      CIN  : in STD_LOGIC
    );
  end component;

begin

  -- Behavioral shift register stages for Spartan-3E compatibility
  SR_STAGE_PROCESS: process(clk)
  begin
    if rising_edge(clk) then
      if chip_enable = '1' then
        -- Stage 0
        shift_regs(0) <= ASR_data_conf(0) & shift_regs(0)(7 downto 1);
        -- Stage 1
        shift_regs(1) <= ASR_data_conf(1) & shift_regs(1)(7 downto 1);
        -- Stage 2
        shift_regs(2) <= ASR_data_conf(2) & shift_regs(2)(7 downto 1);
        -- Stage 3
        shift_regs(3) <= ASR_data_conf(3) & shift_regs(3)(7 downto 1);
      end if;
    end if;
  end process;

  -- Multiplexers to select shift register output based on length config
  ASRQ0 <= shift_regs(0)(to_integer(unsigned(ASR_length_conf(2 downto 0))));
  ASRQ1 <= shift_regs(1)(to_integer(unsigned(ASR_length_conf(5 downto 3))));
  ASRQ2 <= shift_regs(2)(to_integer(unsigned(ASR_length_conf(8 downto 6))));
  ASRQ3 <= shift_regs(3)(to_integer(unsigned(ASR_length_conf(11 downto 9))));

  -- Mode 0 is ADD (SUM = I0 xor I1 xor CIN, COUT = function of I0, I1, CIN)
  alu_inst_0 : ALU
    generic map (ALU_MODE => 0)
    port map (
      I0 => ASRQ0,
      I1 => '1',
      I3 => '0',
      CIN => '0',
      COUT => c_out_0,
      SUM => sum_0
    );

  alu_inst_1 : ALU
    generic map (ALU_MODE => 0)
    port map (
      I0 => ASRQ1,
      I1 => '0',
      I3 => '0',
      CIN => c_out_0,
      COUT => c_out_1,
      SUM => sum_1
    );

  alu_inst_2 : ALU
    generic map (ALU_MODE => 0)
    port map (
      I0 => ASRQ2,
      I1 => '0',
      I3 => '0',
      CIN => c_out_1,
      COUT => c_out_2,
      SUM => sum_2
    );

  alu_inst_3 : ALU
    generic map (ALU_MODE => 0)
    port map (
      I0 => ASRQ3,
      I1 => '0',
      I3 => '0',
      CIN => c_out_2,
      COUT => c_out_3,
      SUM => sum_3
    );

  -- Use top of carry chain as delay race output
  carry_out <= c_out_3;

  -- Flip-flop with set input (replaces FDPE primitive)
  -- Behavioral implementation of Set-dominant flip-flop
  FF_PROCESS: process(clk, carry_out)
  begin
    if carry_out = '1' then
      puf_bit <= '1';  -- Asynchronous set (PRE input)
    elsif rising_edge(clk) then
      if ff_reset = '1' then
        puf_bit <= '0';  -- Synchronous reset via D input
      end if;
    end if;
  end process;

end Behavioral;